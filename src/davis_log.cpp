// ===========================================================================
// davis_log.cpp  —  The serial-message ring buffer (see davis_log.h)
// ===========================================================================

#include "davis_log.h"

DavisLog Log;   // the global object everything prints through

// The ring buffer of completed lines. `head` is the oldest stored line; `count`
// is how many are valid. When full, the newest line overwrites the oldest.
static char     lines[LOG_MAX_LINES][LOG_MAX_LINE_LEN];
static uint16_t head  = 0;
static uint16_t count = 0;

// The line currently being assembled (characters arrive one or a few at a time
// as code prints; we commit a finished line when we see a newline).
static char     building[LOG_MAX_LINE_LEN];
static uint16_t buildLen = 0;

// Move the line we've been assembling into the ring buffer and start fresh.
static void commitLine() {
  uint16_t slot;
  if (count < LOG_MAX_LINES) {
    slot = (head + count) % LOG_MAX_LINES;   // append into a free slot
    count++;
  } else {
    slot = head;                             // full: reuse the oldest slot...
    head = (head + 1) % LOG_MAX_LINES;       // ...and advance "oldest" forward
  }
  memcpy(lines[slot], building, buildLen);
  lines[slot][buildLen] = '\0';
  buildLen = 0;
}

size_t DavisLog::write(uint8_t c) {
  // 1) Always pass the byte straight through to the real serial port, so USB
  //    logging behaves exactly as it always did.
  Serial.write(c);

  // 2) Also fold it into the line we're building for the web buffer.
  if (c == '\r') return 1;                   // ignore carriage returns
  if (c == '\n') { commitLine(); return 1; } // newline ends the current line
  if (buildLen < LOG_MAX_LINE_LEN - 1) {
    building[buildLen++] = (char)c;          // normal character
  }
  // (If a line runs longer than the buffer, the extra characters are dropped
  //  from the stored copy — the full line still went out over serial above.)
  return 1;
}

size_t DavisLog::write(const uint8_t *buf, size_t size) {
  for (size_t i = 0; i < size; i++) write(buf[i]);
  return size;
}

uint16_t logLineCount() { return count; }

const char *logLine(uint16_t i) {
  if (i >= count) return "";
  return lines[(head + i) % LOG_MAX_LINES];
}
