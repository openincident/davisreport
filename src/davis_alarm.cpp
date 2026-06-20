// ===========================================================================
// davis_alarm.cpp  —  The severe-weather threshold logic
// ---------------------------------------------------------------------------
// Plain-English summary of what this does:
//   - It keeps a running total of rain over the last 30 minutes (because the
//     NWS Phoenix rule is "half an inch in 30 minutes or less").
//   - Every time it's called, it compares the current readings to the limits
//     from config.h: 30-minute rain, wind gust, and steady wind.
//   - If any limit is crossed, it raises an alert and remembers WHY.
//   - It "holds" the alert on for a while (ALARM_HOLD_SECONDS) after the weather
//     calms down, so a brief gust doesn't blink past before you notice it.
// ===========================================================================

#include <Arduino.h>
#include "davis_alarm.h"
#include "config.h"

// If an older config.h doesn't define these, fall back to sensible "off" values
// so the code still builds.
#ifndef ALARM_ENABLE
#define ALARM_ENABLE 0
#endif
#ifndef ALARM_RAIN_30MIN_INCHES
#define ALARM_RAIN_30MIN_INCHES 0
#endif
#ifndef ALARM_WIND_GUST_MPH
#define ALARM_WIND_GUST_MPH 0
#endif
#ifndef ALARM_WIND_SUSTAINED_MPH
#define ALARM_WIND_SUSTAINED_MPH 0
#endif
#ifndef ALARM_HOLD_SECONDS
#define ALARM_HOLD_SECONDS 0
#endif

// ---------------------------------------------------------------------------
// The rolling 30-minute rain window.
// ---------------------------------------------------------------------------
// We take one snapshot of the all-time rain counter every minute and keep the
// last 31 snapshots (just over 30 minutes' worth) in a ring buffer. The rain
// that fell "in the last 30 minutes" is simply: the newest counter value minus
// the oldest one we still have on file.
static const uint8_t RAIN_SLOTS = 31;
static uint32_t rainSnapshots[RAIN_SLOTS];
static uint8_t  rainSlotsUsed = 0;     // how many snapshots we've collected (0..31)
static uint8_t  rainHead = 0;          // where the NEXT snapshot will go
static uint32_t lastRainSampleMs = 0;  // when we last took a snapshot

// ---------------------------------------------------------------------------
// The current alert state.
// ---------------------------------------------------------------------------
static bool        active = false;     // is an alert showing right now?
static const char *reason = "";        // why (short text)
static uint32_t    lastTrippedMs = 0;  // last time a threshold was actually crossed
static bool        everTripped = false;

// ---------------------------------------------------------------------------
void alarmInit() {
  rainSlotsUsed = 0;
  rainHead = 0;
  lastRainSampleMs = 0;
  active = false;
  reason = "";
  lastTrippedMs = 0;
  everTripped = false;
}

// How many inches of rain have fallen across the snapshots we currently hold.
static float rainInLast30Min(const DavisData *data) {
  if (rainSlotsUsed == 0) return 0.0f;
  // The oldest snapshot sits `rainSlotsUsed` steps behind where the next one
  // will be written.
  uint8_t oldest = (uint8_t)((rainHead + RAIN_SLOTS - rainSlotsUsed) % RAIN_SLOTS);
  uint32_t clicksSince = data->rainClicksTotal - rainSnapshots[oldest];
  return clicksSince * 0.01f;   // each rain "click" is 0.01 inch
}

// ---------------------------------------------------------------------------
void alarmUpdate(const DavisData *data) {
  // Feature switched off? Make sure we report "no alert" and stop.
  if (!ALARM_ENABLE) {
    active = false;
    reason = "";
    return;
  }

  uint32_t now = millis();

  // --- Take a rain snapshot once per minute. ---
  if (lastRainSampleMs == 0 || (now - lastRainSampleMs) >= 60000UL) {
    lastRainSampleMs = now;
    rainSnapshots[rainHead] = data->rainClicksTotal;
    rainHead = (uint8_t)((rainHead + 1) % RAIN_SLOTS);
    if (rainSlotsUsed < RAIN_SLOTS) rainSlotsUsed++;
  }

  // --- Check each threshold. A threshold of 0 means "don't check this one."
  // The first one that trips wins (it becomes the displayed reason). ---
  bool tripped = false;
  const char *why = "";

  if (!tripped && ALARM_RAIN_30MIN_INCHES > 0 &&
      rainInLast30Min(data) >= (float)ALARM_RAIN_30MIN_INCHES) {
    tripped = true;
    why = "HEAVY RAIN";
  }
  if (!tripped && ALARM_WIND_GUST_MPH > 0 &&
      data->windGustMph >= (float)ALARM_WIND_GUST_MPH) {
    tripped = true;
    why = "WIND GUST";
  }
  if (!tripped && ALARM_WIND_SUSTAINED_MPH > 0 &&
      data->windSpeedMph >= (float)ALARM_WIND_SUSTAINED_MPH) {
    tripped = true;
    why = "HIGH WIND";
  }

  // --- Decide whether to show an alert. ---
  if (tripped) {
    // A threshold is crossed right now.
    active = true;
    reason = why;
    everTripped = true;
    lastTrippedMs = now;
  } else if (everTripped &&
             (now - lastTrippedMs) < (uint32_t)ALARM_HOLD_SECONDS * 1000UL) {
    // Nothing crossed this instant, but we crossed one recently — keep holding
    // the alert (and its last reason) so it doesn't flicker away too fast.
    active = true;
  } else {
    // All clear.
    active = false;
    reason = "";
  }
}

// ---------------------------------------------------------------------------
bool        alarmActive() { return active; }
const char *alarmReason() { return reason; }
