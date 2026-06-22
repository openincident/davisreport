// ===========================================================================
// davis_mqtt.h  —  Sending readings to Home Assistant over WiFi
// ---------------------------------------------------------------------------
// These functions connect the board to your WiFi and to your MQTT broker (the
// messaging system Home Assistant listens to), and then publish the weather
// readings. Thanks to "MQTT discovery," Home Assistant creates all the sensors
// by itself — you don't have to edit any Home Assistant configuration files.
// ===========================================================================

#ifndef DAVIS_MQTT_H
#define DAVIS_MQTT_H

#include "davis_protocol.h"

// Starts WiFi and prepares the MQTT connection. Call once at startup.
void mqttBegin();

// Keeps the connection alive and reconnects if WiFi or MQTT drops. Call this
// often from loop() (it returns quickly).
void mqttLoop();

// Publishes the current readings to Home Assistant. Safe to call as often as
// you like; it only actually sends if connected.
void mqttPublish(const DavisData *data, float rssi, bool radioLocked);

// Keeps WiFi alive if it drops: forces a reconnect, and as a last resort does a
// clean reboot after a long outage. Call often from loop().
void wifiWatchdog();

// Status getters used by the display.
bool wifiIsConnected();
bool mqttIsConnected();

#endif // DAVIS_MQTT_H
