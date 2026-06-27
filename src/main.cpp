// ===========================================================================
// main.cpp  —  The conductor that ties everything together
// ---------------------------------------------------------------------------
// An Arduino-style program has exactly two special functions:
//
//   setup()  runs ONCE when the board powers on. We use it to start the screen,
//            the radio, and the WiFi/MQTT connection.
//
//   loop()   runs OVER AND OVER, forever, as fast as it can. We use it to:
//              - keep WiFi/MQTT alive
//              - check the radio for a new weather message
//              - decode it, show it on screen, and send it to Home Assistant
//
// All the hard work lives in the other files (radio, protocol, display, mqtt);
// this file just calls them in the right order. Read this first to get the big
// picture, then dive into the others for the details.
// ===========================================================================

#include <Arduino.h>
#include "config.h"
#include "davis_protocol.h"
#include "davis_radio.h"
#include "davis_display.h"
#include "davis_mqtt.h"
#include "davis_alarm.h"
#include "davis_web.h"

// Our single, shared record of the latest weather readings. Every part of the
// program reads from or writes to this one structure.
static DavisData weather;

// A scratch buffer to hold the 10 raw bytes of one radio message.
static uint8_t packet[DAVIS_PACKET_LENGTH];

// Remember when we last redrew the screen, so we don't redraw thousands of
// times per second (which would waste effort and flicker).
static uint32_t lastDisplayMs = 0;

// When a packet arrives we light the LED until this time (a brief blink).
static uint32_t ledPulseUntilMs = 0;
#define LED_PULSE_MS 40   // how long each packet "heartbeat" blink lasts

// Remember when we last printed the serial status summary (see loop()).
static uint32_t lastStatusMs = 0;

// ---------------------------------------------------------------------------
// setup(): one-time startup.
// ---------------------------------------------------------------------------
void setup() {
  // Open the USB serial connection for debug messages. Match the speed in
  // platformio.ini (monitor_speed). Watch these with: pio device monitor
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println(F("=== davisreport: Davis weather receiver starting ==="));

  // These "[setup] ..." markers (each followed by Serial.flush(), which waits
  // for the text to actually be sent) let us see exactly how far startup gets
  // if the board ever hangs or reboots. Whatever marker prints LAST is the step
  // that's failing. They're cheap to leave in.
  Serial.println(F("[setup] 1/4 init data")); Serial.flush();

  // Start with a clean, empty set of readings.
  davisInit(&weather);

  // Get the alert logic ready, and set up the LED pin (off to start). The LED is
  // used both for the severe-weather alert and for the per-packet heartbeat.
  alarmInit();
  if (ALARM_LED_ENABLE || LED_BLINK_ON_PACKET) {
    pinMode(PIN_ALERT_LED, OUTPUT);
    digitalWrite(PIN_ALERT_LED, LOW);
  }

  // Turn on the little screen and show a "starting up" message.
  Serial.println(F("[setup] 2/4 starting display")); Serial.flush();
  displayBegin();
  Serial.println(F("[setup] 2/4 display OK")); Serial.flush();

  // Start the radio. If this fails, it's almost always a pin/wiring problem,
  // so we stop here and keep showing the error rather than pretending to work.
  Serial.println(F("[setup] 3/4 starting radio")); Serial.flush();
  if (!radioBegin()) {
    Serial.println(F("FATAL: radio failed to start. Check the pin numbers in config.h."));
    // Halt in a gentle loop, leaving the failure visible on serial.
    while (true) {
      delay(1000);
    }
  }

  // Start joining WiFi and preparing the MQTT (Home Assistant) connection.
  Serial.println(F("[setup] 4/4 starting wifi/mqtt")); Serial.flush();
  mqttBegin();

  // Get the onboard web dashboard ready (it starts serving once WiFi connects).
  webBegin();

  Serial.println(F("Startup complete. Listening for the Davis station..."));
}

// ---------------------------------------------------------------------------
// loop(): runs continuously.
// ---------------------------------------------------------------------------
void loop() {
  // 1. Keep the WiFi + MQTT connection healthy (reconnects if it dropped), and
  //    answer any web-dashboard requests. The watchdog forces WiFi back if the
  //    core's auto-reconnect gives up.
  wifiWatchdog();
  mqttLoop();
  webLoop();

  // 2. Ask the radio whether a new weather message has arrived. This also keeps
  //    the frequency-hopping in sync, so we call it as often as possible.
  //    radioPoll() only returns true for REAL, checksum-verified packets — it
  //    silently discards noise on its own — so anything we get here is good.
  if (radioPoll(packet)) {
    uint32_t rainBefore = weather.rainClicksTotal;
    davisDecode(packet, &weather);     // pull the weather values out of it
    weather.goodPacketCount++;

    // RAIN DIAGNOSTIC (temporary): log every rain-counter message so we can see
    // what's behind any phantom rain. For each rain (type 0xE) packet we print
    // the transmitter ID, the raw 0-127 bucket-tip counter, the signal strength,
    // and the running total — and flag when a tip actually got counted. If we
    // see the counter blip to an odd value (at low RSSI) and the total jump,
    // that's a corrupted packet slipping past the checksum, not real rain.
    if (((packet[0] & 0xF0) >> 4) == 0x0E) {
      Serial.printf("[rain] id=%u raw=%u rssi=%ddBm total=%.2fin%s\n",
                    packet[0] & 0x07, packet[3] & 0x7F, (int)radioGetRssi(),
                    weather.rainClicksTotal * 0.01f,
                    (weather.rainClicksTotal != rainBefore) ? "  <-- TIP COUNTED" : "");
    }

    mqttPublish(&weather, radioGetRssi(), radioIsLocked());  // send to Home Assistant
    ledPulseUntilMs = millis() + LED_PULSE_MS;   // brief LED "heartbeat" blink
  }

  uint32_t now = millis();

  // 3. Re-check the severe-weather thresholds (this also keeps the rolling
  //    30-minute rain total current), and blink the alert LED if we're alarmed.
  alarmUpdate(&weather);
  if (ALARM_LED_ENABLE || LED_BLINK_ON_PACKET) {
    bool ledOn;
    if (ALARM_LED_ENABLE && alarmActive()) {
      ledOn = (now / 150) % 2;          // alert: ~3 Hz flash (takes priority)
    } else if (LED_BLINK_ON_PACKET) {
      ledOn = (now < ledPulseUntilMs);  // brief blink for each received packet
    } else {
      ledOn = false;
    }
    digitalWrite(PIN_ALERT_LED, ledOn ? HIGH : LOW);
  }

  // 4. Refresh the on-screen display about twice a second. We also use this
  //    moment to trigger the periodic "keep-alive" publish to Home Assistant.
  if (now - lastDisplayMs >= 500) {
    lastDisplayMs = now;
    displayShow(&weather, radioIsLocked(), radioGetRssi(),
                wifiIsConnected(), mqttIsConnected(),
                alarmActive(), alarmReason());
    // Re-send readings periodically even with no new packet, so Home Assistant
    // never marks the sensors stale. (mqttPublish decides if it's time.)
    mqttPublish(&weather, radioGetRssi(), radioIsLocked());
    // Keep the web dashboard's current values fresh (it records a history point
    // only every WEB_SAMPLE_SECONDS on its own).
    webSample(&weather, radioGetRssi(), radioIsLocked(),
              alarmActive(), alarmReason());
  }

  // 5. Every few seconds, print a one-line status summary to the serial log.
  //    This is our "dashboard" when running headless: it shows whether we're
  //    locked on, how many good vs. garbled messages we've received (a measure
  //    of reception quality), the signal strength, the current readings, and
  //    whether WiFi/MQTT are connected.
  if (now - lastStatusMs >= 5000) {
    lastStatusMs = now;
    Serial.printf(
      "[status] lock=%s rssi=%ddBm peak=%ddBm good=%lu bad=%lu other=%lu fei=%+dHz ppm=%+.2f | %.1fF %.0f%%RH dp=%.0fF wind=%.0fmph dir=%u gust=%.0f rain=%.2fin | wifi=%s mqtt=%s alert=%s\n",
      radioIsLocked() ? "YES" : "no",
      (int)radioGetRssi(),
      (int)radioGetRssiPeak(),
      (unsigned long)weather.goodPacketCount,
      (unsigned long)radioBadCount(),
      (unsigned long)radioOtherCount(),
      (int)radioGetLastFeiHz(),
      radioGetPpm(),
      weather.tempF, weather.humidityPct,
      davisDewPointF(weather.tempF, weather.humidityPct),
      weather.windSpeedMph,
      weather.windDirDegrees, weather.windGustMph,
      weather.rainClicksTotal * 0.01f,
      wifiIsConnected() ? "Y" : "n",
      mqttIsConnected() ? "Y" : "n",
      alarmActive() ? alarmReason() : "none");
  }
}
