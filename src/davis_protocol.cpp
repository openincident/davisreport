// ===========================================================================
// davis_protocol.cpp  —  Turning 10 raw bytes into real weather readings
// ---------------------------------------------------------------------------
// This is the "translator." The radio hands us 10 raw bytes; the code here
// (a) checks they aren't garbled, and (b) figures out what weather values they
// represent.
//
// The byte layout and the math below come from years of community reverse-
// engineering work (see the credits in README.md). The formulas look a little
// magical, but each one is explained in plain English where it's used.
// ===========================================================================

#include "davis_protocol.h"

// ---------------------------------------------------------------------------
// Set up a "blank" record. Everything starts at zero / "not heard yet."
// ---------------------------------------------------------------------------
void davisInit(DavisData *data) {
  // memset writes zero to every byte of the struct in one shot. Since 0 is the
  // right "empty" value for every field here, that's all we need.
  memset(data, 0, sizeof(DavisData));
}

// ---------------------------------------------------------------------------
// THE CHECKSUM (also called CRC) — is this message intact?
// ---------------------------------------------------------------------------
// Radio is noisy. To catch messages that got scrambled in the air, Davis
// includes a 2-byte "checksum" computed from the message contents. We redo the
// same calculation on the bytes we received and compare. If our number matches
// the checksum the station sent, the message is almost certainly intact.
//
// The specific recipe Davis uses is a well-known one called "CRC-16/CCITT":
//   - start with the number 0
//   - mix in the first 6 data bytes one at a time using the steps below
//   - the station put its own answer in bytes 6 and 7
// If our computed answer equals bytes 6-7, the message is good.
//
// You don't need to understand the bit-twiddling to trust it — it's a standard,
// battle-tested formula. The comments explain the gist.
static uint16_t crc16CcittDavis(const uint8_t *bytes, uint8_t count) {
  uint16_t crc = 0x0000;                 // the running answer, starts at zero
  for (uint8_t n = 0; n < count; n++) {
    crc ^= (uint16_t)bytes[n] << 8;      // fold the next byte into the top of the answer
    for (uint8_t bit = 0; bit < 8; bit++) {
      // For each of the 8 bits: shift left, and if a bit "fell off" the top,
      // mix in the magic CCITT number 0x1021. This scrambling is what makes the
      // checksum sensitive to even a single flipped bit.
      if (crc & 0x8000) {
        crc = (crc << 1) ^ 0x1021;
      } else {
        crc = (crc << 1);
      }
    }
  }
  return crc;
}

bool davisCrcValid(const uint8_t *packet) {
  // Compute the checksum over the first 6 data bytes...
  uint16_t computed = crc16CcittDavis(packet, 6);
  // ...and read the checksum the station actually sent (bytes 6 and 7,
  // most-significant byte first).
  uint16_t sent = ((uint16_t)packet[6] << 8) | packet[7];
  return (computed == sent);
}

// ---------------------------------------------------------------------------
// THE DECODER — pull weather values out of a known-good message.
// ---------------------------------------------------------------------------
void davisDecode(const uint8_t *packet, DavisData *data) {
  uint32_t now = millis();              // current time, for the "freshness" stamps
  data->lastAnyPacket = now;

  // --- The message type lives in the top half of the very first byte. ---
  // One byte holds 8 bits. The "upper nibble" is the top 4 bits. We shift the
  // byte right by 4 to move those top 4 bits down so we can read them as a
  // number from 0 to 15. That number tells us which rotating reading this
  // message carries (temperature, humidity, rain, etc.).
  uint8_t messageType = (packet[0] & 0xF0) >> 4;

  // --- The "battery low" warning is a single flag bit in that first byte. ---
  // Bit value 0x08 is the low-battery flag. If it's set, the ISS is telling us
  // its battery (or solar storage) is running low.
  data->batteryLow = (packet[0] & 0x08) != 0;

  // =========================================================================
  // ALWAYS PRESENT: wind speed and wind direction (in every single message)
  // =========================================================================

  // Wind speed: byte 1 is simply the speed in miles per hour. No math needed.
  data->windSpeedMph = (float)packet[1];

  // Wind direction: byte 2 is a number from 0 to 255 that represents a compass
  // bearing. Multiply by (360/255) to stretch the 0-255 range onto a full
  // 0-360 degree circle. Davis then measures from a different zero point, so we
  // add the conventional 180 degree offset and wrap back into 0-359.
  uint16_t raw = (uint16_t)((packet[2] * 360.0f) / 255.0f);
  data->windDirDegrees = (raw >= 180) ? (raw - 180) : (raw + 180);

  data->lastWindUpdate = now;

  // =========================================================================
  // ROTATING READING: exactly one of these, chosen by the message type.
  // =========================================================================
  switch (messageType) {

    // --- Type 0x2: Solar-panel storage capacitor voltage (a health value) ---
    // Not weather, but useful: a healthy supercap reads roughly 2.5-2.8 V in
    // daylight. It's a 10-bit number split across two bytes, in hundredths of
    // a volt.
    case 0x2: {
      uint16_t rawCap = ((uint16_t)packet[3] << 2) | ((packet[4] & 0xC0) >> 6);
      data->superCapVolts = rawCap / 100.0f;
      break;
    }

    // --- Type 0x5: Rain RATE (how hard it's raining right now) ---
    // This one is unusual: the station sends the TIME BETWEEN rain-bucket tips,
    // and we turn that into a rate. A bigger gap = lighter rain.
    case 0x5: {
      uint16_t rawRate = packet[3] + ((packet[4] & 0x30) * 16);
      if (rawRate == 0) {
        // A raw value of 0 means "no rain right now."
        data->rainRateInHr = 0.0f;
      } else {
        // Two ranges: bit 0x40 picks the heavy-rain scale vs. the light scale.
        // The result "clicksPerHour" is how many 0.01-inch bucket tips per hour.
        float clicksPerHour = ((packet[4] & 0x40) == 0)
                                ? (57600.0f / rawRate)   // heavier rain
                                : (3600.0f  / rawRate);  // lighter rain
        // Each click is 0.01 inch of rain, so inches/hour = clicks/hour * 0.01.
        data->rainRateInHr = clicksPerHour * 0.01f;
      }
      data->lastRainUpdate = now;
      break;
    }

    // --- Type 0x7: Solar radiation (sunlight intensity) ---
    // A 10-bit raw number; the scale factor converts it to watts per square
    // meter. Stations without a solar sensor will just report 0 / noise here.
    case 0x7: {
      uint16_t rawSolar = ((uint16_t)packet[3] << 2) | ((packet[4] & 0xC0) >> 6);
      data->solarWm2 = (uint16_t)(rawSolar * 1.757936f / 100.0f);
      break;
    }

    // --- Type 0x8: Outside temperature ---
    // A 16-bit number (two bytes stuck together) that, divided by 160, gives
    // degrees Fahrenheit.
    case 0x8: {
      int16_t rawTemp = ((int16_t)packet[3] << 8) | packet[4];
      data->tempF = rawTemp / 160.0f;
      data->lastTempUpdate = now;
      break;
    }

    // --- Type 0x9: Wind gust (the strongest wind in the recent window) ---
    // Byte 3 is simply the gust speed in miles per hour.
    case 0x9: {
      data->windGustMph = (float)packet[3];
      break;
    }

    // --- Type 0xA: Outside humidity ---
    // A 10-bit number spread across two bytes, in tenths of a percent.
    // We rebuild the number then divide by 10 to get a normal 0-100 percentage.
    case 0xA: {
      uint16_t rawHum = (((uint16_t)(packet[4] >> 4)) << 8) | packet[3];
      data->humidityPct = rawHum / 10.0f;
      data->lastHumidityUpdate = now;
      break;
    }

    // --- Type 0xE: Rain COUNTER (total bucket tips so far) ---
    // The bottom 7 bits are a counter that climbs 0,1,2,...,127 and then wraps
    // back to 0. We can't read the absolute total directly, so we watch how
    // much it climbs between messages and add that to our own running total.
    case 0xE: {
      static uint8_t  previousCounter = 0xFF;   // 0xFF = "we haven't seen one yet"
      uint8_t currentCounter = packet[3] & 0x7F;
      if (previousCounter != 0xFF) {
        // How many new tips since last time? The "& 0x7F" gives the forward
        // distance around the 0..127 circle, which handles the wrap from 127
        // back to 0 cleanly (e.g. 125 -> 2 counts as 5 new tips).
        uint8_t forward = (currentCounter - previousCounter) & 0x7F;
        // IMPORTANT: only count SMALL forward steps. Real rain adds at most a
        // few tips between messages. If the counter appears to jump way forward
        // (>= 64), that's actually the counter glitching slightly BACKWARD
        // (a backward step of 1 looks like a forward step of 127). We ignore
        // those, otherwise a tiny glitch would fake ~1.27 inches of rain.
        if (forward < 64) {
          data->rainClicksTotal += forward;
        }
      }
      previousCounter = currentCounter;
      data->lastRainUpdate = now;
      break;
    }

    // --- Any other type: nothing we decode (UV sensors, etc.) ---
    default:
      // We still counted the wind reading above, so the message wasn't wasted.
      break;
  }
}

// ---------------------------------------------------------------------------
// Dew point — calculated from temperature and humidity.
// ---------------------------------------------------------------------------
// This uses the well-known "Magnus formula." You don't need to follow the math;
// it's a standard, accurate approximation used by weather software everywhere.
// The constants a and b are the published Magnus coefficients for water vapor.
float davisDewPointF(float tempF, float humidityPct) {
  // Before the station has reported humidity it reads 0, which would break the
  // math (you can't take the log of 0). Clamp it to a sane range first.
  if (humidityPct < 1.0f)   humidityPct = 1.0f;
  if (humidityPct > 100.0f) humidityPct = 100.0f;

  // The formula works in Celsius, so convert the temperature in.
  float tC = (tempF - 32.0f) * 5.0f / 9.0f;

  const float a = 17.625f;
  const float b = 243.04f;   // degrees Celsius

  // gamma is an intermediate term combining temperature and humidity.
  float gamma = logf(humidityPct / 100.0f) + (a * tC) / (b + tC);

  // Solve for the dew point in Celsius, then convert back to Fahrenheit.
  float dewC = (b * gamma) / (a - gamma);
  return dewC * 9.0f / 5.0f + 32.0f;
}
