// ===========================================================================
// davis_alarm.h  —  Deciding when the weather is "report-worthy"
// ---------------------------------------------------------------------------
// This watches the live readings and decides whether they've crossed the
// severe-weather thresholds you set in config.h (the SkyWarn-style limits). It
// doesn't draw anything or flash anything itself — it just answers two
// questions: "is there an alert right now?" and "what kind?". The display, LED,
// and Home Assistant code use those answers.
// ===========================================================================

#ifndef DAVIS_ALARM_H
#define DAVIS_ALARM_H

#include "davis_protocol.h"

// Resets the alarm logic to a clean "no alert" state. Call once at startup.
void alarmInit();

// Re-checks the readings against the thresholds. Call this regularly (we call
// it about twice a second). It also keeps the rolling 30-minute rain total
// up to date, which is why it needs to run continuously, not just now and then.
void alarmUpdate(const DavisData *data);

// True if an alert is active right now (a threshold is crossed, OR one was
// crossed recently and we're still holding the alert for ALARM_HOLD_SECONDS).
bool alarmActive();

// A short, human-readable reason for the current alert — e.g. "HEAVY RAIN",
// "WIND GUST", "HIGH WIND". Returns an empty string when there's no alert.
const char *alarmReason();

#endif // DAVIS_ALARM_H
