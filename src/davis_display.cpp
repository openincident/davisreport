// ===========================================================================
// davis_display.cpp  —  Painting the OLED screen
// ---------------------------------------------------------------------------
// We use the "U8g2" library, which knows how to draw text and shapes on the
// SSD1306 screen built into the LilyGO board. The screen is 128 pixels wide and
// 64 pixels tall — small, so we keep the layout simple:
//
//     +--------------------------------+
//     | Davis Weather        [WiFi/MQ] |   <- title + connection icons
//     | 72.4 F            48% RH        |   <- temperature and humidity
//     | Wind  6 mph  NW                |   <- wind speed and direction
//     | RX -58dBm  good 1234  lock     |   <- radio health / packet counts
//     +--------------------------------+
//
// Everything is converted to your chosen units (set USE_IMPERIAL_UNITS in
// config.h) right before it's drawn.
// ===========================================================================

#include <U8g2lib.h>
#include <Wire.h>           // the low-level I2C library (used to probe for the screen)
#include "davis_display.h"
#include "config.h"

// If an older config.h doesn't define this, assume the screen is wanted.
#ifndef ENABLE_OLED
#define ENABLE_OLED 1
#endif

// Remembers whether we actually found a screen. If we didn't (some board
// variants wire it differently or omit it), every draw call simply does
// nothing, so a missing screen can never stall the rest of the program.
static bool displayPresent = false;

// Create the screen object. This particular line says: an SSD1306 128x64 screen
// connected over I2C (a 2-wire bus), with the reset, clock, and data pins taken
// from config.h. "_F_" means we keep a full-screen buffer in memory and draw it
// all at once (no flicker).
// We pass U8X8_PIN_NONE for the reset pin on purpose: we pulse the reset line
// (GPIO16) ourselves in displayBegin() BEFORE any I2C activity. Letting U8g2
// drive the reset during its begin() can re-trigger the boot-loop on this board,
// so we take that job away from it.
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(
    U8G2_R0,           // no rotation
    U8X8_PIN_NONE,     // reset handled manually (see displayBegin)
    PIN_OLED_SCL,      // clock pin
    PIN_OLED_SDA);     // data pin

// ---------------------------------------------------------------------------
// Helper: turn a compass bearing in degrees into a short label like "NW".
// ---------------------------------------------------------------------------
// We split the 360 degree circle into 16 slices of 22.5 degrees each and pick
// the matching label. Adding half a slice before dividing rounds to the nearest.
static const char *bearingToLabel(uint16_t degrees) {
  static const char *labels[16] = {
    "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
    "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"
  };
  uint8_t slice = (uint8_t)(((degrees + 11) / 22) % 16);
  return labels[slice];
}

// ---------------------------------------------------------------------------
// displayBegin(): wake up the screen.
// ---------------------------------------------------------------------------
void displayBegin() {
  // If the screen is switched off in config.h, don't touch the I2C bus at all —
  // just run headless. (This is the safe escape hatch for boards whose screen
  // stalls the bus at startup.)
  if (!ENABLE_OLED) {
    Serial.println(F("[display] OLED disabled in config.h; running headless."));
    displayPresent = false;
    return;
  }

  // STEP 1: hardware-reset the screen FIRST, before any I2C traffic. On this
  // board the SSD1306's reset wire is GPIO16. Until we pulse it, the screen
  // powers up in an undefined state and can hold the I2C data line low, which
  // jams the bus and hangs the whole board (this was the cause of the earlier
  // boot loop). The pulse is: drive it low, wait, drive it high, wait.
  // NOTE: we deliberately do NOT drive the "OLED reset" pin (GPIO16) on this
  // board. Doing so freezes the ESP32 — on this module GPIO16 is tied to
  // internal memory (PSRAM), not a safe reset line. The screen resets fine on
  // power-up without us touching it.

  // STEP 2: bring up the I2C bus and make sure a screen actually answers. We
  // give it a short timeout and send a quick "are you there?" to the screen's
  // address, so that a missing or mis-wired screen can never wedge us — if
  // nothing answers we just skip the screen and run headless.
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  Wire.setTimeOut(50);                 // never wait more than 50 ms on the bus
  Wire.beginTransmission(0x3C);        // 0x3C is the SSD1306 screen's I2C address
  if (Wire.endTransmission() != 0) {   // non-zero = nobody answered
    Serial.println(F("[display] no OLED found at 0x3C — running without a screen."));
    displayPresent = false;
    return;
  }

  // STEP 3: the screen answered, so set it up. (U8g2 wants the address in its
  // 8-bit form, hence the shift.)
  displayPresent = true;
  oled.setI2CAddress(0x3C << 1);
  oled.begin();
  oled.setFontMode(1);            // draw text without erasing a box behind it
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x12_tr);
  oled.drawStr(0, 12, "Davis Weather");
  oled.drawStr(0, 30, "Starting up...");
  oled.sendBuffer();              // push what we drew to the actual screen
  Serial.println(F("[display] OLED ready."));
}

// ---------------------------------------------------------------------------
// displayShow(): redraw everything with the latest values.
// ---------------------------------------------------------------------------
void displayShow(const DavisData *data, bool radioOk, float rssi,
                 bool wifiOk, bool mqttOk, bool alarm, const char *alarmReason) {
  // If there's no screen, there's nothing to draw — just return quietly.
  if (!displayPresent) return;

  // A reusable little text buffer for formatting numbers into strings.
  char line[28];

  // This flips every time we're called (~twice a second). We use it to make the
  // alert banner BLINK: on one frame we show the bright alert bar, on the next
  // we show the normal title, which reads as a flashing warning.
  static bool blinkPhase = false;
  blinkPhase = !blinkPhase;

  oled.clearBuffer();   // start from a blank screen each time

  oled.setFont(u8g2_font_6x12_tr);

  // --- Title row, OR a flashing alert banner ---
  if (alarm && blinkPhase) {
    // Alert + "on" half of the blink: draw a solid white bar across the top and
    // print the reason in black on top of it (inverse text) — impossible to miss.
    oled.drawBox(0, 0, 128, 14);
    oled.setDrawColor(0);                       // draw in "black" (un-lit pixels)
    snprintf(line, sizeof(line), "! %s !", alarmReason);
    oled.drawStr(2, 11, line);
    oled.setDrawColor(1);                       // back to normal "white" drawing
  } else {
    // Normal title (also shown on the "off" half of the blink, so it flashes).
    oled.drawStr(0, 11, "Davis Weather");
    // Connection icons on the top-right: "W" for WiFi, "M" for MQTT. We draw
    // them only when connected, so a missing letter instantly shows what's down.
    if (wifiOk) oled.drawStr(104, 11, "W");
    if (mqttOk) oled.drawStr(116, 11, "M");
  }

  // A thin line under the title to separate it from the readings.
  oled.drawHLine(0, 14, 128);

  // --- Temperature (big, on the left) ---
  if (USE_IMPERIAL_UNITS) {
    snprintf(line, sizeof(line), "%.1f F", data->tempF);
  } else {
    float tempC = (data->tempF - 32.0f) * 5.0f / 9.0f;
    snprintf(line, sizeof(line), "%.1f C", tempC);
  }
  oled.setFont(u8g2_font_9x15_tr);
  oled.drawStr(0, 32, line);

  // --- Right column (smaller): humidity on top, dew point below it ---
  oled.setFont(u8g2_font_6x12_tr);
  snprintf(line, sizeof(line), "RH %.0f%%", data->humidityPct);
  oled.drawStr(80, 26, line);

  float dewF = davisDewPointF(data->tempF, data->humidityPct);
  if (USE_IMPERIAL_UNITS) {
    snprintf(line, sizeof(line), "DP %.0fF", dewF);
  } else {
    snprintf(line, sizeof(line), "DP %.0fC", (dewF - 32.0f) * 5.0f / 9.0f);
  }
  oled.drawStr(80, 38, line);

  // --- Wind speed and direction ---
  oled.setFont(u8g2_font_6x12_tr);
  if (USE_IMPERIAL_UNITS) {
    snprintf(line, sizeof(line), "Wind %.0f mph %s",
             data->windSpeedMph, bearingToLabel(data->windDirDegrees));
  } else {
    float windKmh = data->windSpeedMph * 1.60934f;
    snprintf(line, sizeof(line), "Wind %.0f kmh %s",
             windKmh, bearingToLabel(data->windDirDegrees));
  }
  oled.drawStr(0, 47, line);

  // --- Bottom status row: radio health and packet counts ---
  // (The "RX ...dBm" form means we're locked; "searching..." means we're not —
  // so we don't need a separate "LOCK" word, which ran off the right edge.)
  if (radioOk) {
    snprintf(line, sizeof(line), "RX %ddBm  ok:%lu",
             (int)rssi, (unsigned long)data->goodPacketCount);
  } else {
    snprintf(line, sizeof(line), "searching...  ok:%lu",
             (unsigned long)data->goodPacketCount);
  }
  oled.drawStr(0, 62, line);

  // Push everything we drew to the physical screen in one go.
  oled.sendBuffer();
}
