// ===========================================================================
// davis_log.h  —  Capture serial messages so the web page can show them
// ---------------------------------------------------------------------------
// "Log" is a drop-in stand-in for "Serial". Anything you print with it goes to
// the USB serial port EXACTLY as before, AND is also kept in a small in-memory
// ring buffer of the most recent lines. The web dashboard reads that buffer and
// shows it in a scrolling box, so you can watch the board's debug log from a
// browser even when no USB cable is attached.
//
// Usage: write Log.print(...) / Log.printf(...) / Log.println(...) wherever you
// would have written Serial.print*. (Serial.begin()/Serial.flush() stay on
// Serial — those aren't print calls.) Nothing else changes.
// ===========================================================================

#ifndef DAVIS_LOG_H
#define DAVIS_LOG_H

#include <Arduino.h>

// The number of recent lines we keep, and the most characters we store per line
// (longer lines are simply clipped in the buffer; the full line still goes to
// the real serial port). The [status] line is ~180 chars, so 200 keeps it whole.
// 60 x 200 bytes is about 12 KB of RAM.
#define LOG_MAX_LINES     60
#define LOG_MAX_LINE_LEN  200

// A Print-compatible object: use it just like Serial for printing.
class DavisLog : public Print {
 public:
  size_t write(uint8_t c) override;
  size_t write(const uint8_t *buf, size_t size) override;
};

extern DavisLog Log;

// How many lines are currently buffered (0..LOG_MAX_LINES).
uint16_t logLineCount();

// Fetch buffered line number `i` (0 = oldest, count-1 = newest). Returns a
// pointer to a null-terminated string owned by the buffer; copy it if you need
// to keep it. Returns "" for an out-of-range index.
const char *logLine(uint16_t i);

#endif // DAVIS_LOG_H
