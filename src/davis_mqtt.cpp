// ===========================================================================
// davis_mqtt.cpp  —  WiFi + MQTT + automatic Home Assistant setup
// ---------------------------------------------------------------------------
// WHAT THIS DOES, IN PLAIN ENGLISH
//
// 1. Joins your WiFi network.
// 2. Connects to your MQTT broker (the little "post office" that Home Assistant
//    reads from — usually the Mosquitto add-on running on Home Assistant).
// 3. The first time it connects, it sends Home Assistant a set of "here's what I
//    am" messages (called "discovery"). Home Assistant reads those and creates
//    a Davis Weather Station device with a temperature sensor, humidity sensor,
//    wind sensors, and so on — automatically. You don't edit any YAML.
// 4. From then on, it publishes the actual readings as a single small JSON
//    message whenever the weather updates. Each Home Assistant sensor knows how
//    to pick its own value out of that JSON.
//
// MQTT TOPICS WE USE (a "topic" is just a named mailbox):
//   davis_iss/state    -> the JSON with all current readings
//   davis_iss/status   -> "online" or "offline" (so HA knows if we're alive)
//   homeassistant/.../config -> the one-time "here's what I am" messages
// (The "davis_iss" part comes from DEVICE_ID in your config.h.)
// ===========================================================================

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "davis_mqtt.h"
#include "davis_alarm.h"
#include "config.h"

// The network plumbing: a basic network connection wrapped by an MQTT client.
static WiFiClient   wifiClient;
static PubSubClient mqtt(wifiClient);

// We only send the "here's what I am" discovery messages once per connection.
static bool discoverySent = false;

// Remember when we last did a full publish, so we can re-send periodically even
// if nothing changed (keeps Home Assistant from greying-out the sensors).
static uint32_t lastPublishMs = 0;

// Topic names, built once from DEVICE_ID. (Plenty of room for long names.)
static char stateTopic[64];
static char statusTopic[64];

// ---------------------------------------------------------------------------
// Helper: send ONE Home Assistant "discovery" message describing one sensor.
// ---------------------------------------------------------------------------
// Home Assistant listens on special "homeassistant/.../config" topics. When it
// receives one of these, it creates (or updates) a sensor. We describe each
// sensor: its name, its unit, what kind of value it is, and how to find its
// value inside our JSON state message.
//
//   component   -> "sensor" for normal readings, "binary_sensor" for on/off
//   objectId    -> a short unique id for this sensor (e.g. "temperature")
//   name        -> the friendly label shown in Home Assistant
//   deviceClass -> HA's category (e.g. "temperature") for icons/units; "" = none
//   unit        -> the unit string (e.g. "°F"); "" = none
//   valueKey    -> which field in our JSON to read (e.g. "temp")
//   stateClass  -> "measurement", "total_increasing", or "" — helps HA graph it
//   diagnostic  -> true to tuck it under "Diagnostic" instead of the main view
static void sendDiscovery(const char *component, const char *objectId,
                          const char *name, const char *deviceClass,
                          const char *unit, const char *valueKey,
                          const char *stateClass, bool diagnostic) {
  // Build the JSON describing this sensor.
  JsonDocument doc;
  doc["name"] = name;

  // A globally-unique id so Home Assistant doesn't create duplicates.
  char uniqueId[80];
  snprintf(uniqueId, sizeof(uniqueId), "%s_%s", DEVICE_ID, objectId);
  doc["unique_id"] = uniqueId;

  doc["state_topic"] = stateTopic;

  // This little template tells HA "pull this field out of the JSON state."
  char valueTemplate[64];
  snprintf(valueTemplate, sizeof(valueTemplate), "{{ value_json.%s }}", valueKey);
  doc["value_template"] = valueTemplate;

  // Mark us online/offline so HA shows "unavailable" if the board goes away.
  doc["availability_topic"] = statusTopic;
  doc["payload_available"] = "online";
  doc["payload_not_available"] = "offline";

  if (strlen(deviceClass) > 0) doc["device_class"] = deviceClass;
  if (strlen(unit) > 0)        doc["unit_of_measurement"] = unit;
  if (strlen(stateClass) > 0)  doc["state_class"] = stateClass;
  if (diagnostic)              doc["entity_category"] = "diagnostic";

  // For a binary (on/off) sensor, spell out what counts as "on" vs "off."
  if (strcmp(component, "binary_sensor") == 0) {
    doc["payload_on"] = "1";
    doc["payload_off"] = "0";
  }

  // Group every sensor under ONE device in Home Assistant by giving them all
  // the same "device" block with the same identifier.
  JsonObject device = doc["device"].to<JsonObject>();
  JsonArray ids = device["identifiers"].to<JsonArray>();
  ids.add(DEVICE_ID);
  device["name"] = DEVICE_NAME;
  device["manufacturer"] = "Davis Instruments";
  device["model"] = "Vantage Pro2 (received via LilyGO T3 v1.6.1)";

  // Turn the JSON into text.
  char payload[640];
  size_t len = serializeJson(doc, payload, sizeof(payload));

  // The topic Home Assistant watches for this sensor's definition.
  char topic[96];
  snprintf(topic, sizeof(topic), "homeassistant/%s/%s_%s/config",
           component, DEVICE_ID, objectId);

  // Publish it "retained" so Home Assistant still finds it even if HA restarts
  // later (the broker keeps the last retained message on the topic).
  mqtt.publish(topic, (const uint8_t *)payload, len, true /* retained */);
}

// ---------------------------------------------------------------------------
// Send ALL the discovery messages (called once right after we connect).
// ---------------------------------------------------------------------------
static void sendAllDiscovery() {
  // Pick unit strings based on whether you chose Imperial or Metric in config.h.
  const char *tempUnit = USE_IMPERIAL_UNITS ? "°F" : "°C";
  const char *windUnit = USE_IMPERIAL_UNITS ? "mph" : "km/h";
  const char *rateUnit = USE_IMPERIAL_UNITS ? "in/h" : "mm/h";
  const char *rainUnit = USE_IMPERIAL_UNITS ? "in" : "mm";

  // --- The main weather readings ---
  sendDiscovery("sensor", "temperature", "Temperature", "temperature", tempUnit, "temp", "measurement", false);
  sendDiscovery("sensor", "humidity",    "Humidity",    "humidity",    "%",      "humidity", "measurement", false);
  sendDiscovery("sensor", "wind_speed",  "Wind Speed",  "wind_speed",  windUnit, "wind_speed", "measurement", false);
  sendDiscovery("sensor", "wind_gust",   "Wind Gust",   "wind_speed",  windUnit, "wind_gust", "measurement", false);
  sendDiscovery("sensor", "wind_dir",    "Wind Direction", "",         "°",      "wind_dir", "measurement", false);
  sendDiscovery("sensor", "rain_rate",   "Rain Rate",   "precipitation_intensity", rateUnit, "rain_rate", "measurement", false);
  sendDiscovery("sensor", "rain_total",  "Rain Total",  "precipitation", rainUnit, "rain_total", "total_increasing", false);
  sendDiscovery("sensor", "solar",       "Solar Radiation", "irradiance", "W/m²", "solar", "measurement", false);

  // --- The severe-weather alert ---
  // A binary (on/off) sensor with device_class "safety", so Home Assistant
  // treats "on" as the unsafe/alert state. Build automations on this. The
  // companion "Alert Reason" text sensor says which threshold tripped.
  sendDiscovery("binary_sensor", "alert", "Weather Alert", "safety", "", "alert", "", false);
  sendDiscovery("sensor", "alert_reason", "Alert Reason", "", "", "alert_reason", "", false);

  // --- Diagnostics (health/signal info, tucked away in HA) ---
  sendDiscovery("sensor", "supercap",    "Supercap Voltage", "voltage", "V",   "supercap", "measurement", true);
  sendDiscovery("sensor", "rssi",        "Signal Strength",  "signal_strength", "dBm", "rssi", "measurement", true);
  sendDiscovery("binary_sensor", "battery_low", "Battery Low", "battery", "", "battery_low", "", true);

  Serial.println(F("[mqtt] sent Home Assistant discovery; sensors should appear now."));
}

// ---------------------------------------------------------------------------
// Try to (re)connect to the MQTT broker. Returns true once connected.
// ---------------------------------------------------------------------------
static bool mqttConnect() {
  // The "last will" is a message the broker sends FOR us if we vanish without
  // saying goodbye — it marks us "offline" so Home Assistant notices.
  bool ok = mqtt.connect(DEVICE_ID, MQTT_USER, MQTT_PASSWORD,
                         statusTopic, 0 /*qos*/, true /*retained*/, "offline");
  if (ok) {
    Serial.println(F("[mqtt] connected to broker."));
    // Announce we're online (retained, so HA sees it any time).
    mqtt.publish(statusTopic, "online", true);
    // (Re)send the discovery messages on every fresh connection.
    sendAllDiscovery();
    discoverySent = true;
  } else {
    Serial.print(F("[mqtt] connect failed, will retry. state="));
    Serial.println(mqtt.state());
  }
  return ok;
}

// ---------------------------------------------------------------------------
// mqttBegin(): join WiFi and get the MQTT client ready.
// ---------------------------------------------------------------------------
void mqttBegin() {
  // Build our topic names from DEVICE_ID once.
  snprintf(stateTopic,  sizeof(stateTopic),  "%s/state",  DEVICE_ID);
  snprintf(statusTopic, sizeof(statusTopic), "%s/status", DEVICE_ID);

  // Start joining WiFi (this happens in the background; we check on it later).
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print(F("[wifi] connecting to "));
  Serial.println(WIFI_SSID);

  // Point the MQTT client at the broker. The bigger buffer is needed because
  // the discovery messages are larger than PubSubClient's small default.
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(768);
}

// ---------------------------------------------------------------------------
// mqttLoop(): keep WiFi + MQTT alive. Call often.
// ---------------------------------------------------------------------------
void mqttLoop() {
  // If MQTT isn't connected, try to reconnect — but only every few seconds so
  // we don't hammer the broker. We can only connect once WiFi is up.
  static uint32_t lastReconnectAttempt = 0;
  if (!mqtt.connected()) {
    discoverySent = false;
    if (WiFi.status() == WL_CONNECTED) {
      uint32_t now = millis();
      if (now - lastReconnectAttempt > 3000) {
        lastReconnectAttempt = now;
        mqttConnect();
      }
    }
  } else {
    // Connected — let the client do its housekeeping (keep-alives, etc.).
    mqtt.loop();
  }
}

// ---------------------------------------------------------------------------
// mqttPublish(): send the current readings as one JSON message.
// ---------------------------------------------------------------------------
void mqttPublish(const DavisData *data, float rssi, bool radioLocked) {
  // Don't bother if we're not connected.
  if (!mqtt.connected()) return;

  // Re-send at most a few times per second even if called rapidly, and force a
  // re-send every MQTT_REPUBLISH_SECONDS so sensors never look stale.
  uint32_t now = millis();
  bool periodic = (now - lastPublishMs) > ((uint32_t)MQTT_REPUBLISH_SECONDS * 1000UL);
  if (!periodic && (now - lastPublishMs) < 1000) return;
  lastPublishMs = now;

  // Build the JSON that every Home Assistant sensor reads from.
  JsonDocument doc;

  // Convert each value into your chosen units as we add it.
  if (USE_IMPERIAL_UNITS) {
    doc["temp"]       = roundf(data->tempF * 10) / 10.0f;          // °F
    doc["wind_speed"] = (int)data->windSpeedMph;                    // mph
    doc["wind_gust"]  = (int)data->windGustMph;                     // mph
    doc["rain_rate"]  = roundf(data->rainRateInHr * 100) / 100.0f;  // in/h
    doc["rain_total"] = roundf(data->rainClicksTotal * 0.01f * 100) / 100.0f; // in
  } else {
    doc["temp"]       = roundf((data->tempF - 32) * 5 / 9 * 10) / 10.0f;       // °C
    doc["wind_speed"] = roundf(data->windSpeedMph * 1.60934f);                 // km/h
    doc["wind_gust"]  = roundf(data->windGustMph * 1.60934f);                  // km/h
    doc["rain_rate"]  = roundf(data->rainRateInHr * 25.4f * 10) / 10.0f;       // mm/h
    doc["rain_total"] = roundf(data->rainClicksTotal * 0.254f * 10) / 10.0f;   // mm
  }

  // These are the same regardless of unit system.
  doc["humidity"]    = roundf(data->humidityPct * 10) / 10.0f;
  doc["wind_dir"]    = data->windDirDegrees;
  doc["solar"]       = data->solarWm2;
  doc["supercap"]    = roundf(data->superCapVolts * 100) / 100.0f;
  doc["battery_low"] = data->batteryLow ? 1 : 0;
  doc["rssi"]        = (int)rssi;

  // The severe-weather alert: a 1/0 flag plus a short reason for Home Assistant.
  doc["alert"]        = alarmActive() ? 1 : 0;
  doc["alert_reason"] = alarmActive() ? alarmReason() : "none";

  // Turn it into text and publish (retained, so HA shows last value on restart).
  char payload[384];
  size_t len = serializeJson(doc, payload, sizeof(payload));
  mqtt.publish(stateTopic, (const uint8_t *)payload, len, true);
}

// ---------------------------------------------------------------------------
// Simple status getters used by the display.
// ---------------------------------------------------------------------------
bool wifiIsConnected() { return WiFi.status() == WL_CONNECTED; }
bool mqttIsConnected() { return mqtt.connected(); }
