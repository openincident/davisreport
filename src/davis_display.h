// ===========================================================================
// davis_display.h  —  Drawing the readings on the little OLED screen
// ---------------------------------------------------------------------------
// The LilyGO board has a small (0.96 inch) black-and-white screen. These two
// functions turn it on and paint the latest weather readings onto it.
// ===========================================================================

#ifndef DAVIS_DISPLAY_H
#define DAVIS_DISPLAY_H

#include "davis_protocol.h"   // for the DavisData struct we display

// Turns on the screen. Call once at startup.
void displayBegin();

// Redraws the whole screen with the latest data and connection status.
//   data        = the current weather readings
//   radioOk     = are we locked onto the station's signal?
//   rssi        = signal strength of the last message (dBm)
//   wifiOk      = are we connected to WiFi?
//   mqttOk      = are we connected to the MQTT broker / Home Assistant?
//   alarm       = is a severe-weather alert active? (flashes the top banner)
//   alarmReason = short text for the alert (e.g. "HEAVY RAIN")
void displayShow(const DavisData *data, bool radioOk, float rssi,
                 bool wifiOk, bool mqttOk, bool alarm, const char *alarmReason);

#endif // DAVIS_DISPLAY_H
