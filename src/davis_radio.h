// ===========================================================================
// davis_radio.h  —  Listening to the Davis station with the SX1276 radio
// ---------------------------------------------------------------------------
// This file lists the tools for controlling the radio chip on the LilyGO board.
// The real work is in davis_radio.cpp, where there's a long plain-English
// explanation of "frequency hopping" and how we keep up with it.
//
// The big idea: the Davis station doesn't sit on one radio frequency. It jumps
// between 51 different frequencies in a fixed, repeating order, sending one
// short message on each before jumping to the next. To catch every message, our
// radio has to jump along in lockstep — tuning to the next frequency just
// before the station broadcasts on it. The code here manages that dance.
// ===========================================================================

#ifndef DAVIS_RADIO_H
#define DAVIS_RADIO_H

#include <Arduino.h>
#include "davis_protocol.h"   // for DAVIS_PACKET_LENGTH

// Starts up the radio chip and tunes it to begin searching for the station.
// Returns true if the radio hardware initialized correctly, false if not
// (a false usually means a wiring/pin problem — check config.h).
bool radioBegin();

// Call this VERY OFTEN from the main loop (many times per second).
// It does two jobs:
//   1. Checks whether a new message arrived; if so, copies its 10 raw bytes
//      into `packetOut` and returns true.
//   2. Manages the frequency-hopping: jumping to the next frequency after each
//      message, and recovering if it loses track of the station.
// Returns true exactly when a fresh 10-byte message is ready in packetOut.
bool radioPoll(uint8_t *packetOut);

// Returns the signal strength of the most recently received message, in dBm.
// (dBm is a radio loudness scale; closer to 0 is stronger. Around -60 is a
// strong nearby signal; -100 or lower is weak/marginal.)
float radioGetRssi();

// Returns true when we are successfully "locked on" and following the station's
// hopping pattern; false while we're still searching for it.
bool radioIsLocked();

#endif // DAVIS_RADIO_H
