// ===========================================================================
// davis_ota.h  —  Over-the-air firmware updates (flash over WiFi, no USB)
// ---------------------------------------------------------------------------
// This lets you upload new firmware to the board over your WiFi network instead
// of plugging in a USB cable. It's handy once the board is mounted somewhere
// awkward — and on this board the USB serial link is flaky on long sessions, so
// OTA is the more reliable way to update it.
//
// The mechanics (ArduinoOTA): the board quietly listens for an update on the
// network (port 3232, announced as "davis" over mDNS). From your computer you
// run the SAME PlatformIO upload you already use, just pointed over WiFi:
//
//     pio run -e lilygo-t3-ota -t upload
//
// It's password-protected (set OTA_PASSWORD in config.h). The new firmware is
// written to the board's SPARE program slot, so a failed/interrupted update
// can't brick it — the board simply keeps running the old firmware until a good
// update completes, then reboots into it. Your stored history is untouched.
// ===========================================================================

#ifndef DAVIS_OTA_H
#define DAVIS_OTA_H

// Call once from setup(). (Cheap: the actual network listener starts later, the
// first time WiFi is connected — see otaLoop.)
void otaBegin();

// Call often from loop(). Brings the OTA listener up when WiFi connects, then
// services any incoming update. Does nothing while WiFi is down.
void otaLoop();

#endif // DAVIS_OTA_H
