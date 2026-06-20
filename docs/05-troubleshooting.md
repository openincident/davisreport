# 5. Troubleshooting

Work down the list — the checks are ordered from most common to least. The board's debug log (via `pio device monitor`) is your best friend; most problems announce themselves there.

---

## The board never says "locked on" / no weather on the screen

This means the radio isn't catching the station's signal.

1. **Wrong frequency board (the #1 cause).** A 433 or 868 MHz board cannot hear a US 915 MHz Davis station. Re-read [docs/01](01-hardware-and-wiring.md) and confirm you have the **915 MHz** version. This is by far the most common reason for total silence.
2. **No antenna / loose antenna.** Make sure the antenna is screwed on snugly, and that it's a 915 MHz antenna.
3. **Be patient — up to ~2 minutes.** On first power-up the board parks on one channel and waits for the station's hop cycle to reach it. Give it a couple of minutes before worrying.
4. **Too far from the sensors.** Move the board closer to your outdoor ISS, or at least to where your Davis console gets a good signal. Thick walls, metal, and distance all hurt.
5. **Wrong STATION_ID.** If `STATION_ID` in `config.h` doesn't match your station (remember: console "Station 1" = `0`), the timing will be off and tracking will be flaky. Double-check it.
6. **It locks, then loses it repeatedly.** If the log shows "locked on" followed by "lost the station" over and over, the signal is marginal — improve placement/antenna. You can also try widening the receiver: in `src/davis_radio.cpp`, bump the `4.8f` frequency-deviation argument in `beginFSK(...)` up toward `9.5f` and/or raise `MISS_GUARD_MS`.

## The radio says "FAILED to start" at boot

The ESP32 can't talk to the radio chip — almost always a pin mismatch.

- Confirm you copied `config.h.example` to `config.h` and didn't change the pin numbers.
- If you have a different board revision, check its documentation for the correct SX1276 pins (SCK, MISO, MOSI, CS, RST, DIO0, DIO1) and update the "Hardware pin map" section of `config.h`.

## The screen stays blank or shows nothing

- Confirm the board actually has an OLED (some bare modules don't).
- Check `PIN_OLED_SDA`, `PIN_OLED_SCL`, and `PIN_OLED_RST` in `config.h` match your board.
- The startup text ("Starting up...") should appear within a second of power-on; if even that's missing, it's a screen wiring/pin issue, not a weather/radio issue.

## WiFi won't connect

The log shows it stuck on `[wifi] connecting to ...`.

- Double-check `WIFI_SSID` and `WIFI_PASSWORD` (case-sensitive).
- The ESP32 only supports **2.4 GHz** WiFi, not 5 GHz. Make sure you're giving it a 2.4 GHz network name.
- Move it closer to the router for the first connection.

## Sensors don't appear in Home Assistant

- Make sure the **MQTT broker** (Mosquitto) is installed and running, and the **MQTT integration** is configured — see [docs/04](04-home-assistant.md).
- Check `MQTT_HOST`, `MQTT_USER`, and `MQTT_PASSWORD` in `config.h`. The log will show `[mqtt] connect failed` with a state number if the login is wrong.
- In Home Assistant, use **MQTT → Configure → Listen to a topic** and subscribe to `davis_iss/#`. If you see messages there, the board is talking and the issue is on the Home Assistant side; if you see nothing, the board isn't connecting.
- Discovery messages are sent **once per connection**. If you were fiddling, just reset the board (unplug/replug) to make it re-send them.

## Some values look wrong or stuck

- **Temperature/humidity/rain update in turns, not all at once.** Remember each message carries only one of these. It can take 10–30 seconds for every value to refresh after a cold start. That's normal.
- **Wind direction seems offset.** The direction math uses a standard offset, but vanes vary slightly. If it's consistently off, compare against your Davis console and adjust the offset in the wind-direction section of `src/davis_protocol.cpp`.
- **Solar/UV looks odd.** Solar scaling is the least-certain part of the reverse-engineered protocol, and stations without a solar sensor will report noise here. If you don't have a solar sensor, ignore that value.

## Still stuck?

Open the debug log with `pio device monitor`, copy what you see around the problem, and check it against the expected healthy startup in [docs/02](02-flashing.md). The difference usually points right at the culprit.
