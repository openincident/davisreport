// ===========================================================================
// davis_radio.cpp  —  The radio + the "frequency hopping" follower
// ---------------------------------------------------------------------------
// PLAIN-ENGLISH OVERVIEW
//
// Imagine the Davis station and its console playing a game of catch, but they
// keep moving to 51 numbered spots in a field in a fixed, memorized order. They
// throw a ball (a radio message) from each spot, about every 2.5 seconds, then
// both move to the next spot. Because the order is fixed and memorized, the
// catcher always knows where the next throw is coming from.
//
// Our board is an extra catcher trying to join the game. We don't get told
// where the thrower currently is, so first we stand on spot #0 and wait. The
// thrower cycles through all 51 spots, so sooner or later it lands on spot #0
// and we catch a ball. The instant we do, we know exactly where the thrower is
// in the memorized order — so from then on we move to the next spot just before
// each throw and catch every one. That's "tracking."
//
// If we ever miss a few throws in a row (someone walked in front of us, radio
// interference, etc.), we give up trying to follow blindly and go back to
// standing on spot #0 to re-find the thrower. That's "re-acquiring."
//
// In radio terms: the 51 "spots" are 51 frequencies; "moving to the next spot"
// is re-tuning the radio; and the fixed order is the published Davis hopping
// pattern. All the numbers below come from the community reverse-engineering
// work credited in the README.
//
// THE RADIO CHIP: this board (LilyGO T3 v1.6.1) carries either a Semtech SX1276
// or an SX1262 — you choose which in config.h. Both can do plain FSK
// ("frequency-shift keying" — sending bits by nudging the frequency up or
// down), which is exactly what Davis uses. The two chips need slightly
// different setup calls, handled by the small #if blocks below; the hopping
// logic and everything else is identical for both.
// ===========================================================================

#include <RadioLib.h>
#include "davis_radio.h"
#include "config.h"

// ---------------------------------------------------------------------------
// THE 51 FREQUENCIES (United States 915 MHz band), in megahertz.
// ---------------------------------------------------------------------------
// These are listed in numerical order (lowest to highest). They are NOT the
// order the station visits them — that's the hop pattern further down. Think of
// this as the map of where all 51 "spots" physically are.
//
// (If you are in EUROPE, your Davis uses the 868 MHz band and a different list.
//  You would replace this whole table and the hop pattern with the EU values.)
static const float CHANNEL_FREQ_MHZ[51] = {
  902.355835f, 902.857585f, 903.359336f, 903.861086f, 904.362837f, 904.864587f,
  905.366338f, 905.868088f, 906.369839f, 906.871589f, 907.373340f, 907.875090f,
  908.376841f, 908.878591f, 909.380342f, 909.882092f, 910.383843f, 910.885593f,
  911.387344f, 911.889094f, 912.390845f, 912.892595f, 913.394346f, 913.896096f,
  914.397847f, 914.899597f, 915.401347f, 915.903098f, 916.404848f, 916.906599f,
  917.408349f, 917.910100f, 918.411850f, 918.913601f, 919.415351f, 919.917102f,
  920.418852f, 920.920603f, 921.422353f, 921.924104f, 922.425854f, 922.927605f,
  923.429355f, 923.931106f, 924.432856f, 924.934607f, 925.436357f, 925.938108f,
  926.439858f, 926.941609f, 927.443359f
};

// ---------------------------------------------------------------------------
// THE HOP PATTERN — the order the station visits those 51 frequencies.
// ---------------------------------------------------------------------------
// Each number is an index into CHANNEL_FREQ_MHZ above. So the station starts on
// frequency #0, then jumps to frequency #19, then #41, and so on, wrapping back
// to the start after all 51. This exact sequence is baked into every Davis ISS.
//
// Handy fact we rely on: the pattern STARTS with frequency #0. So if we sit on
// frequency #0 and catch a message, we know we're at the very start of the
// pattern (position 0) and can follow from there.
static const uint8_t HOP_PATTERN[51] = {
   0, 19, 41, 25,  8, 47, 32, 13, 36, 22,  3, 29, 44, 16,  5, 27, 38, 10,
  49, 21,  2, 30, 42, 14, 48,  7, 24, 34, 45,  1, 17, 39, 26,  9, 31, 50,
  37, 12, 20, 33,  4, 43, 28, 15, 35,  6, 40, 11, 23, 46, 18
};
static const uint8_t NUM_CHANNELS = 51;

// ---------------------------------------------------------------------------
// HOW OFTEN THE STATION TRANSMITS
// ---------------------------------------------------------------------------
// The gap between messages depends on the station's transmitter ID. The formula
// is 2.5625 seconds, plus 1/16 of a second for each ID step above zero.
// We compute it once at startup, in milliseconds.
static uint32_t transmitIntervalMs = 0;

// How long after the expected arrival time we wait before declaring a message
// "missed." Radios and clocks aren't perfectly precise, so we allow a little
// slack. (milliseconds) — tunable via HOP_MISS_GUARD_MS in config.h.
#ifndef HOP_MISS_GUARD_MS
#define HOP_MISS_GUARD_MS 1000
#endif
static const uint32_t MISS_GUARD_MS = HOP_MISS_GUARD_MS;

// How many messages in a row we'll tolerate missing while still "coasting"
// (following the pattern blindly). After this many, we give up and re-acquire
// from scratch on frequency #0 — tunable via HOP_MAX_MISSES in config.h.
#ifndef HOP_MAX_MISSES
#define HOP_MAX_MISSES 10
#endif
static const uint16_t MAX_CONSECUTIVE_MISSES = HOP_MAX_MISSES;

// ---------------------------------------------------------------------------
// THE RADIO OBJECT (provided by the RadioLib library)
// ---------------------------------------------------------------------------
// This represents the radio chip. We tell it which pins connect us to it.
// The two chip versions wire one control pin differently, so we build the
// matching object based on your choice in config.h:
//   - SX1262 uses a "BUSY" line (the chip's way of saying "hold on, I'm working")
//   - SX1276 uses a "DIO0" line instead
#if defined(RADIO_CHIP_SX1262)
static SX1262 radio = new Module(PIN_LORA_CS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY);
#else
static SX1276 radio = new Module(PIN_LORA_CS, PIN_LORA_DIO0, PIN_LORA_RST, PIN_LORA_DIO1);
#endif

// ---------------------------------------------------------------------------
// SHARED STATE between the main code and the interrupt handler.
// ---------------------------------------------------------------------------
// "Interrupt" = the radio chip can tap the ESP32 on the shoulder the instant a
// message arrives, by toggling the DIO1 pin. The tiny function below runs at
// that moment. It must do as little as possible, so it just sets a flag that
// the main loop notices later. "volatile" tells the compiler this flag can
// change at any time (from the interrupt), so don't optimize it away.
static volatile bool packetWaitingFlag = false;

// This function is the "shoulder tap" handler. Keep it minimal. IRAM_ATTR puts
// it in fast internal memory so it can run even during flash operations.
IRAM_ATTR static void onPacketArrived() {
  packetWaitingFlag = true;
}

// Flip the order of the 8 bits in a byte (so 0b10110000 becomes 0b00001101).
// We need this because the Davis station sends each byte least-significant-bit
// first, while the radio hands bytes to us most-significant-bit first — so
// every received byte arrives mirrored, and this puts it right.
static uint8_t reverseByte(uint8_t b) {
  b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;   // swap the two nibbles (4-bit halves)
  b = (b & 0xCC) >> 2 | (b & 0x33) << 2;   // swap adjacent bit-pairs
  b = (b & 0xAA) >> 1 | (b & 0x55) << 1;   // swap adjacent bits
  return b;
}

// ---------------------------------------------------------------------------
// Our own tracking variables.
// ---------------------------------------------------------------------------
enum RadioState {
  STATE_ACQUIRING,   // searching for the station on frequency #0
  STATE_TRACKING     // locked on and following the hop pattern
};
static RadioState state = STATE_ACQUIRING;

static uint8_t  patternPosition = 0;     // where we think we are in HOP_PATTERN (0..50)
static uint32_t nextExpectedMs = 0;      // when we expect the next message to arrive
static uint16_t consecutiveMisses = 0;   // how many we've missed in a row
static float    lastRssi = -999.0f;      // signal strength of the last thing we received
static uint32_t badPacketCount = 0;      // receptions that FAILED the Davis checksum (usually noise)
static float    rssiPeak = -999.0f;      // strongest live signal seen since the last status print
static uint32_t otherStationCount = 0;   // valid packets ignored because they're a DIFFERENT station

// ---------------------------------------------------------------------------
// Small helper: tune the radio to a given position in the hop pattern and
// start listening there.
// ---------------------------------------------------------------------------
static void tuneToPatternPosition(uint8_t position) {
  uint8_t channelIndex = HOP_PATTERN[position];        // which frequency #?
  float   freqMhz      = CHANNEL_FREQ_MHZ[channelIndex];
  radio.standby();                 // pause receiving while we change frequency
  radio.setFrequency(freqMhz);     // tune to the new frequency
  radio.startReceive();            // resume listening
}

// ---------------------------------------------------------------------------
// radioBegin(): power up and configure the radio to match Davis's settings.
// ---------------------------------------------------------------------------
bool radioBegin() {
  // Compute the transmit interval from the station's transmitter ID.
  // (2,562,500 microseconds + 62,500 per ID step) converted to milliseconds.
  transmitIntervalMs = (2562500UL + (uint32_t)STATION_ID * 62500UL) / 1000UL;

  // Point the SPI bus (how the ESP32 talks to the radio) at the right pins.
  SPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_CS);

  // Start the radio in "FSK" mode with the exact settings the Davis station
  // uses. The arguments, in order, are:
  //   - starting frequency (MHz): begin on hop-pattern position 0's frequency
  //   - bit rate: 19.2 kilobits per second
  //   - frequency deviation: 9.5 kHz (how far the frequency nudges per bit).
  //       NOTE: many guides copy "4.8 kHz" from one early project, but the real
  //       on-air deviation is ~9.5 kHz. Using 4.8 makes the demodulator produce
  //       garbage and the receiver never locks onto a real packet.
  //   - receiver bandwidth: 25 kHz (how wide a slice of spectrum we listen to)
  //   - transmit power: 10 (we only listen, so this is irrelevant)
  //   - preamble length: 16 (a lead-in pattern; default is fine for receiving)
  float startFreq = CHANNEL_FREQ_MHZ[HOP_PATTERN[0]];
#if defined(RADIO_CHIP_SX1262)
  // The SX1262 takes two extra arguments: the TCXO (temperature-controlled
  // crystal) voltage — 1.8 V here — and the power-regulator choice. If the
  // radio fails to start, the TCXO voltage is the first thing to try changing
  // (some boards use 0, meaning "no TCXO", or 3.3). See docs/05-troubleshooting.
  // The SX1262 has no separate AFC bandwidth, so we use a wider receive filter
  // (50 kHz) to swallow the station's frequency offset instead.
  int status = radio.beginFSK(startFreq, 19.2f, 9.5f, 50.0f, 10, 16, 1.8f, false);
#else
  // The SX1276 uses the same FSK settings but has no TCXO argument.
  int status = radio.beginFSK(startFreq, 19.2f, 9.5f, 25.0f, 10, 16);
#endif
  if (status != RADIOLIB_ERR_NONE) {
    // A non-zero status means the radio didn't start. Usually a wiring/pin
    // problem, or the TCXO voltage above. Report the code for troubleshooting.
    Serial.print(F("[radio] FAILED to start, error code: "));
    Serial.println(status);
    return false;
  }

  // --- Match the rest of Davis's framing so we recognize its messages. ---

  // The "sync word" is a fixed marker the station puts at the start of every
  // message so receivers know "a real message starts here." Davis uses 0xCB 0x89.
  uint8_t syncWord[] = { 0xCB, 0x89 };
  radio.setSyncWord(syncWord, 2);

  // Every Davis message is exactly 10 bytes, so use fixed-length mode.
  radio.fixedPacketLengthMode(DAVIS_PACKET_LENGTH);

  // Davis shapes its signal with a Gaussian filter (this is "GFSK"). Matching
  // that shaping (BT=0.5) on our side improves how cleanly we lock onto it.
  radio.setDataShaping(RADIOLIB_SHAPING_0_5);

  // The last few settings are spelled slightly differently on the two chips.
  // In both cases we're saying: turn OFF the chip's built-in checksum (we do
  // our own in software) and don't scramble/whiten the data.
#if defined(RADIO_CHIP_SX1262)
  radio.setCRC(0);                    // 0 = no hardware checksum
  radio.setWhitening(false);          // Davis does not whiten its data
  radio.setRxBoostedGainMode(true);   // extra receive sensitivity (SX1262 feature)
#else
  radio.setCRC(false);                // no hardware checksum
  radio.setEncoding(RADIOLIB_ENCODING_NRZ);  // send bits as-is: no whitening/manchester

  // ENABLE AFC (automatic frequency correction). This is essential: Davis
  // transmitters sit ~20-28 kHz off their nominal frequency (crystal tolerance),
  // so without AFC the signal lands at the edge of our filter and we only ever
  // hear noise. The radio measures each incoming burst's offset and retunes to
  // match. We give AFC a wider bandwidth (50 kHz) than the normal receive
  // filter so it can first "find" the off-center signal, then lock it in.
  radio.setAFCBandwidth(50.0);
  radio.setAFC(true);
  radio.setAFCAGCTrigger(RADIOLIB_SX127X_RX_TRIGGER_PREAMBLE_DETECT);
#endif

  // Tell the radio to tap our handler the instant a full message arrives.
  radio.setPacketReceivedAction(onPacketArrived);

  // Begin listening on frequency #0 and start out in "searching" mode.
  state = STATE_ACQUIRING;
  patternPosition = 0;
  consecutiveMisses = 0;
  radio.startReceive();

  Serial.println(F("[radio] started OK; searching for the station on channel 0..."));
  return true;
}

// ---------------------------------------------------------------------------
// radioPoll(): the heartbeat. Call this constantly from loop().
// ---------------------------------------------------------------------------
bool radioPoll(uint8_t *packetOut) {
  uint32_t now = millis();

  // Continuously sample the LIVE signal strength (not just when something trips
  // the sync word). We remember the strongest reading since the last status
  // print. This is a diagnostic: if a real Davis burst ever reaches the
  // antenna, this peak will jump far above the noise floor even if we don't
  // manage to decode it — which tells us "signal is present" vs. "antenna is
  // hearing nothing." The arguments mean: read the current RSSI, and don't
  // disturb the radio's receive mode while doing it.
  float curRssi = radio.getRSSI(false, true);
  if (curRssi > rssiPeak) rssiPeak = curRssi;

  // === CASE 1: the radio received SOMETHING. =============================
  if (packetWaitingFlag) {
    packetWaitingFlag = false;     // clear the flag for next time

    // Pull the 10 raw bytes out of the radio.
    int status = radio.readData(packetOut, DAVIS_PACKET_LENGTH);
    lastRssi = radio.getRSSI();    // note how strong the signal was (handy even for noise)

    // If the read itself errored (rare), just resume listening where we are.
    if (status != RADIOLIB_ERR_NONE) {
      radio.startReceive();
      return false;
    }

    // Un-mirror every byte (Davis sends bits least-significant first; the radio
    // gives them to us most-significant first). After this, the bytes are the
    // true Davis values that the checksum and decoder expect.
    for (uint8_t i = 0; i < DAVIS_PACKET_LENGTH; i++) packetOut[i] = reverseByte(packetOut[i]);

    // ----- THE KEY CHECK -----
    // The radio can trigger on plain noise that happens to match Davis's short
    // 2-byte start marker. Those "packets" are garbage and FAIL the checksum.
    // We must verify the checksum HERE, before we treat this as the real
    // station — otherwise a burst of noise would drag us off into chasing a
    // station that isn't there, and we'd never catch the actual one.
    if (!davisCrcValid(packetOut)) {
      badPacketCount++;
      // It's noise. Don't change our lock/searching state and don't move — just
      // keep listening on the SAME channel. If we're still searching, that means
      // we stay parked on channel 0 waiting for a genuine packet.
      radio.startReceive();
      return false;
    }

    // ----- Is it OUR station? -----
    // The lowest 3 bits of byte 0 are the transmitter ID. The Davis console only
    // listens to its own ISS, and so do we: another Davis station nearby also
    // passes the checksum, and letting its independent rain counter mix with ours
    // invents phantom rain. So we accept only our configured station (STATION_ID)
    // and ignore the rest — without resyncing to their timing or hopping with them.
    if ((packetOut[0] & 0x07) != (STATION_ID & 0x07)) {
      otherStationCount++;
      radio.startReceive();
      return false;
    }

    // ----- A REAL, checksum-valid packet from OUR station! -----
    if (state == STATE_ACQUIRING) {
      // We were parked on channel 0 waiting; a valid packet here means the
      // station is at the very start of its hop pattern, so now we know exactly
      // where it is and can follow along.
      Serial.println(F("[radio] locked on! Now following the hop pattern."));
    }
    state = STATE_TRACKING;
    consecutiveMisses = 0;

    // Schedule when the next message should arrive, then hop ahead to the next
    // frequency so we're waiting there before the station transmits.
    nextExpectedMs  = now + transmitIntervalMs;
    patternPosition = (patternPosition + 1) % NUM_CHANNELS;
    tuneToPatternPosition(patternPosition);

    return true;   // tell the caller a fresh, valid packet is ready in packetOut
  }

  // === CASE 2: no message yet — decide whether we've "missed" one. ========
  if (state == STATE_TRACKING) {
    // Have we waited past the expected arrival time (plus our slack)?
    if ((int32_t)(now - (nextExpectedMs + MISS_GUARD_MS)) >= 0) {
      consecutiveMisses++;

      if (consecutiveMisses >= MAX_CONSECUTIVE_MISSES) {
        // We've lost the thread. Go back to searching from frequency #0.
        Serial.println(F("[radio] lost the station; re-searching from channel 0."));
        state = STATE_ACQUIRING;
        patternPosition = 0;
        consecutiveMisses = 0;
        tuneToPatternPosition(patternPosition);
      } else {
        // Assume the station transmitted and we simply didn't hear it. Hop to
        // the next frequency anyway ("dead reckoning") and keep the schedule
        // anchored to the original timing so small errors don't pile up.
        nextExpectedMs += transmitIntervalMs;
        patternPosition = (patternPosition + 1) % NUM_CHANNELS;
        tuneToPatternPosition(patternPosition);
      }
    }
  }
  // While ACQUIRING we just keep listening patiently on frequency #0; the
  // station will land there within one full cycle (~2 minutes worst case).

  return false;
}

// ---------------------------------------------------------------------------
// Simple status getters used by the display.
// ---------------------------------------------------------------------------
float    radioGetRssi()  { return lastRssi; }
bool     radioIsLocked() { return state == STATE_TRACKING; }
uint32_t radioBadCount() { return badPacketCount; }
uint32_t radioOtherCount() { return otherStationCount; }

// Returns the strongest live signal seen since the last call, then resets the
// peak. (So each status line shows the peak during that 5-second window.)
float radioGetRssiPeak() {
  float p = rssiPeak;
  rssiPeak = -999.0f;
  return p;
}
