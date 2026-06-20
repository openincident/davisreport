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
#include "davis_display.h"
#include "config.h"

// Create the screen object. This particular line says: an SSD1306 128x64 screen
// connected over I2C (a 2-wire bus), with the reset, clock, and data pins taken
// from config.h. "_F_" means we keep a full-screen buffer in memory and draw it
// all at once (no flicker).
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(
    U8G2_R0,           // no rotation
    PIN_OLED_RST,      // reset pin
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
  oled.begin();
  oled.setFontMode(1);            // draw text without erasing a box behind it
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x12_tr);
  oled.drawStr(0, 12, "Davis Weather");
  oled.drawStr(0, 30, "Starting up...");
  oled.sendBuffer();              // push what we drew to the actual screen
}

// ---------------------------------------------------------------------------
// displayShow(): redraw everything with the latest values.
// ---------------------------------------------------------------------------
void displayShow(const DavisData *data, bool radioOk, float rssi,
                 bool wifiOk, bool mqttOk) {
  // A reusable little text buffer for formatting numbers into strings.
  char line[28];

  oled.clearBuffer();   // start from a blank screen each time

  // --- Title row ---
  oled.setFont(u8g2_font_6x12_tr);
  oled.drawStr(0, 11, "Davis Weather");

  // Connection icons on the top-right: "W" for WiFi, "M" for MQTT. We draw them
  // only when connected, so a missing letter instantly shows what's down.
  if (wifiOk) oled.drawStr(104, 11, "W");
  if (mqttOk) oled.drawStr(116, 11, "M");

  // A thin line under the title to separate it from the readings.
  oled.drawHLine(0, 14, 128);

  // --- Temperature (big-ish) and humidity ---
  if (USE_IMPERIAL_UNITS) {
    snprintf(line, sizeof(line), "%.1f F", data->tempF);
  } else {
    float tempC = (data->tempF - 32.0f) * 5.0f / 9.0f;
    snprintf(line, sizeof(line), "%.1f C", tempC);
  }
  oled.setFont(u8g2_font_9x15_tr);
  oled.drawStr(0, 32, line);

  // Humidity on the right side of the same row.
  snprintf(line, sizeof(line), "%.0f%%", data->humidityPct);
  oled.drawStr(86, 32, line);

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
  if (radioOk) {
    snprintf(line, sizeof(line), "RX %ddBm  ok:%lu  LOCK",
             (int)rssi, (unsigned long)data->goodPacketCount);
  } else {
    snprintf(line, sizeof(line), "searching...  ok:%lu",
             (unsigned long)data->goodPacketCount);
  }
  oled.drawStr(0, 62, line);

  // Push everything we drew to the physical screen in one go.
  oled.sendBuffer();
}
