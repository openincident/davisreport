// ===========================================================================
// davis_protocol.h  —  Understanding the Davis weather packets
// ---------------------------------------------------------------------------
// This file describes the SHAPE of the data we receive from the Davis station
// and lists the tools (functions) for working with it. The actual logic lives
// in davis_protocol.cpp.
//
// Background, in plain English:
//   Your Davis outdoor sensor suite ("ISS") broadcasts a tiny radio message
//   every ~2.5 seconds. Each message is exactly 10 bytes long. Two of those
//   bytes are a checksum used to detect garbled messages. The remaining bytes
//   carry the weather data.
//
//   Here's the catch: a single 10-byte message does NOT contain every reading.
//   Every message always includes wind speed and wind direction, but it only
//   carries ONE of the other readings (temperature OR humidity OR rain OR ...).
//   The station rotates through them. So we keep a running record (the
//   "DavisData" struct below) and update whichever field arrived in the latest
//   message, leaving the others untouched until their turn comes around again.
// ===========================================================================

#ifndef DAVIS_PROTOCOL_H
#define DAVIS_PROTOCOL_H

#include <Arduino.h>

// How many bytes are in one Davis radio message. Always 10.
#define DAVIS_PACKET_LENGTH 10

// ---------------------------------------------------------------------------
// DavisData: our running record of the latest known weather values.
// ---------------------------------------------------------------------------
// We fill this in over time as different message types arrive. Each value has
// a "fresh" timestamp (millis() when we last updated it) so the display and
// Home Assistant can tell how recently a value was actually heard.
struct DavisData {
  // -- Values that arrive in EVERY message --
  float    windSpeedMph;       // current wind speed, miles per hour
  uint16_t windDirDegrees;     // wind direction, 0-359 (0 = North, 90 = East)

  // -- Values that rotate through the messages (one per message) --
  float    tempF;              // outside temperature, degrees Fahrenheit
  float    humidityPct;        // outside relative humidity, 0-100 %
  float    windGustMph;        // strongest recent gust, miles per hour
  uint16_t solarWm2;           // solar radiation, watts per square meter (0 if no sensor)
  uint32_t rainClicksTotal;    // running count of rain "clicks" (each click = 0.01 in)
  float    rainRateInHr;       // current rain rate, inches per hour
  float    superCapVolts;      // solar-panel storage capacitor voltage (health indicator)

  // -- Status flags --
  bool     batteryLow;         // true if the ISS reports its battery is low

  // -- Bookkeeping: when did we last update each group of values? (millis) --
  uint32_t lastWindUpdate;
  uint32_t lastTempUpdate;
  uint32_t lastHumidityUpdate;
  uint32_t lastRainUpdate;
  uint32_t lastAnyPacket;      // when ANY valid packet last arrived

  // -- Counters, handy for the on-screen health display --
  uint32_t goodPacketCount;    // total valid (checksum-passed) packets
  uint32_t badPacketCount;     // total packets that failed the checksum
};

// ---------------------------------------------------------------------------
// The functions other parts of the program can call.
// ---------------------------------------------------------------------------

// Sets every field of a DavisData record to a sensible "nothing heard yet"
// starting state. Call this once at startup.
void davisInit(DavisData *data);

// Checks whether a freshly received 10-byte message is intact (not garbled)
// using the same checksum math the real Davis console uses.
// Returns true if the message passed the checksum, false if it's corrupt.
bool davisCrcValid(const uint8_t *packet);

// Takes one VALID 10-byte message and updates the matching field(s) in `data`.
// (Always updates wind; updates one other reading depending on message type.)
// Call davisCrcValid() FIRST and only call this if it returned true.
void davisDecode(const uint8_t *packet, DavisData *data);

// Works out the DEW POINT (in °F) from a temperature (°F) and relative humidity
// (%). The dew point is the temperature the air would have to cool to before
// its moisture starts condensing — a more intuitive "how muggy is it" number
// than humidity alone. It's a calculation, not a sensor reading, so we derive
// it from the temperature and humidity the station already reports.
float davisDewPointF(float tempF, float humidityPct);

#endif // DAVIS_PROTOCOL_H
