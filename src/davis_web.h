// ===========================================================================
// davis_web.h  —  A little website served by the board itself
// ---------------------------------------------------------------------------
// When enabled, the board runs a small web server on your WiFi network. Open
// http://davis.local (or the board's IP) in any browser and you get a page with
// the current conditions and graphs of the weather since the board started up.
//
// The history is kept in the board's memory (a fixed-size "ring buffer"), so it
// covers a limited window (24 hours by default) and resets when the board
// reboots — that's the "since uptime" behavior. For long-term history that
// survives reboots, Home Assistant is the place (see docs/04-home-assistant.md).
// ===========================================================================

#ifndef DAVIS_WEB_H
#define DAVIS_WEB_H

#include "davis_protocol.h"

// Starts the web server (and the "davis.local" name). Call once at startup,
// AFTER WiFi has been started. Prints the address to the serial log.
void webBegin();

// Keep the web server responsive. Call this often from loop() (it returns
// quickly when nobody's asking for a page).
void webLoop();

// Records one point into the history graphs. Call this periodically (the code
// in main.cpp does it every WEB_SAMPLE_SECONDS). `rssi` is the signal strength
// and `locked`/`alert`/`alertReason` describe the current status, which the
// page shows in its header.
void webSample(const DavisData *data, float rssi, bool locked,
               bool alert, const char *alertReason);

// Immediately save the live history ring to flash. Call this before an
// intentional reboot so the most recent points aren't lost.
void webPersistNow();

#endif // DAVIS_WEB_H
