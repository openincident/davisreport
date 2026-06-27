// ===========================================================================
// davis_ota.cpp  —  Over-the-air firmware updates (see davis_ota.h)
// ===========================================================================

#include <ArduinoOTA.h>      // ships with the ESP32 Arduino core; no extra library
#include <WiFi.h>
#include "davis_ota.h"
#include "config.h"
#include "davis_log.h"

// Let an older config.h leave OTA on by default, and supply a fallback password
// so a missing setting can't accidentally publish an UNPROTECTED update port.
#ifndef OTA_ENABLE
#define OTA_ENABLE 1
#endif
#ifndef OTA_PASSWORD
#define OTA_PASSWORD "change-me"
#endif

// We can only start the network listener once WiFi is connected, and we must
// re-start it if WiFi drops and comes back. This flag tracks whether the
// listener is currently up.
static bool otaStarted = false;

void otaBegin() {
  // Nothing to do yet — otaLoop() brings the listener up the moment WiFi is
  // connected. (Starting it here would fail, since WiFi isn't up during setup.)
}

void otaLoop() {
#if OTA_ENABLE
  // No WiFi -> no OTA. If we had a listener, forget it so we cleanly re-announce
  // when WiFi returns (mirrors how the web server handles reconnects).
  if (WiFi.status() != WL_CONNECTED) {
    otaStarted = false;
    return;
  }

  // First loop after WiFi came up: configure and start the OTA listener once.
  if (!otaStarted) {
    // Same friendly name as the web dashboard, so it shows up as "davis" on the
    // network. (mDNS is already running from the web module; ArduinoOTA just
    // adds its own service to it.)
    ArduinoOTA.setHostname(WEB_HOSTNAME);

    // REQUIRE A PASSWORD. Without this, anyone on the network could push
    // firmware to the board. The matching password is given at upload time
    // (see platformio.ini / the OTA env).
    ArduinoOTA.setPassword(OTA_PASSWORD);

    // Progress/status messages on the serial log, so an update is easy to watch
    // if you do happen to have a cable attached.
    ArduinoOTA.onStart([]() {
      Log.println(F("[ota] update incoming — pausing normal work..."));
    });
    ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
      Log.printf("[ota] %u%%\r", total ? (done * 100u) / total : 0u);
    });
    ArduinoOTA.onEnd([]() {
      Log.println(F("\n[ota] update complete — rebooting into new firmware."));
    });
    ArduinoOTA.onError([](ota_error_t err) {
      Log.printf("[ota] FAILED (error %u) — board keeps running old firmware.\n",
                    (unsigned)err);
    });

    ArduinoOTA.begin();
    otaStarted = true;
    Log.print(F("[ota] ready — flash over WiFi with: pio run -e lilygo-t3-ota -t upload  (host "));
    Log.print(WEB_HOSTNAME);
    Log.println(F(".local)"));
  }

  // Service any in-progress update. When an update is actually transferring,
  // this call blocks until it finishes — that's fine, it's brief, and the
  // weather/web/MQTT work resumes (or the board reboots) right after.
  ArduinoOTA.handle();
#endif
}
