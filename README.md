# davisreport

**Read a Davis Vantage Pro2 weather station out of the air with a $15 radio board, show the readings on a tiny screen, and send them to Home Assistant.**

This project turns a [LilyGO T3-S3](https://lilygo.cc/products/t3s3-v1-0) (a small ESP32-S3 board with a built-in radio and a little OLED screen) into a receiver for your Davis weather station. It listens to the radio signal your outdoor sensors already broadcast, decodes the temperature/wind/rain/humidity, displays them on the board's screen, and publishes them to [Home Assistant](https://www.home-assistant.io/) over MQTT.

You do **not** need to touch your existing Davis console — this is a completely separate, passive listener that hears the same broadcast the console hears.

---

## What you get

- 📡 **Direct over-the-air reception** of your Davis outdoor sensors (no cables to the console, no cloud, no subscription).
- 🖥️ **Live readings on the board's OLED screen** — current temperature, wind, humidity, and a signal/health indicator.
- 🏠 **Automatic Home Assistant integration** — sensors appear by themselves (via "MQTT discovery"), grouped under one device. No YAML editing required on the Home Assistant side.

```
   Your Davis outdoor sensors            LilyGO T3 board                 Home Assistant
   (Integrated Sensor Suite)
   ┌───────────────────────┐    915 MHz   ┌──────────────────┐   WiFi    ┌──────────────┐
   │  temp, wind, rain,    │  ─ radio ──▶ │  decode + show   │  ──MQTT─▶ │  weather     │
   │  humidity, solar...   │              │  on OLED screen  │           │  dashboard   │
   └───────────────────────┘              └──────────────────┘           └──────────────┘
```

---

## How it works (the short version)

Your Davis outdoor unit (Davis calls it the **ISS**, the Integrated Sensor Suite) sends a tiny radio packet about every 2.5 seconds. To make the signal robust, it **hops around 51 different frequencies** in the 915 MHz band in a fixed, repeating pattern — this is called *frequency hopping*. 

Our board has to play along: it tunes to the next frequency in the pattern just in time to catch each packet, the same way the Davis console does. Once it catches a packet, it checks the packet isn't corrupted, pulls the weather values out of it, and then shows and publishes them.

There's a much longer, friendlier explanation in **[docs/03-how-it-works.md](docs/03-how-it-works.md)**.

---

## Getting started

The full walkthrough lives in the `docs/` folder. In order:

1. **[docs/01-hardware-and-wiring.md](docs/01-hardware-and-wiring.md)** — what board to buy, how to make sure it's the 915 MHz version, and the antenna.
2. **[docs/02-flashing.md](docs/02-flashing.md)** — installing the build tool and loading the program onto the board.
3. **[docs/03-how-it-works.md](docs/03-how-it-works.md)** — the weather-radio protocol explained in plain English (optional, but interesting).
4. **[docs/04-home-assistant.md](docs/04-home-assistant.md)** — getting the sensors to show up in Home Assistant, plus an example dashboard.
5. **[docs/05-troubleshooting.md](docs/05-troubleshooting.md)** — what to do when it isn't hearing anything.

### The 60-second version

```bash
# 1. Install the build tool (PlatformIO)
pip install platformio

# 2. Copy the settings template and fill in your WiFi + MQTT details
cp src/config.h.example src/config.h
#    ...then edit src/config.h in your editor...

# 3. Plug the board in over USB, compile, and flash it
pio run --target upload

# 4. Watch it work
pio device monitor
```

If everything is set up right, within a minute or two you'll see weather readings on the OLED and a new **Davis Weather Station** device in Home Assistant.

---

## Important: get the 915 MHz board

This code is set up for the **United States 915 MHz** Davis band. The LilyGO T3 is sold in 433 MHz, 868 MHz, and 915 MHz versions, and they look almost identical. A 433 or 868 MHz board **will not hear** a US Davis station well. Before you buy or flash, confirm you have the **915 MHz** variant — details in [docs/01-hardware-and-wiring.md](docs/01-hardware-and-wiring.md).

(If you're in Europe, your Davis is on 868 MHz; see the note in the hardware doc about switching the frequency table.)

---

## Credits

This project stands on the shoulders of people who reverse-engineered the Davis radio protocol years ago. Huge thanks to:

- **[dekay/DavisRFM69](https://github.com/dekay/DavisRFM69)** — the original Davis RF protocol reverse-engineering and register settings.
- **[bemasher/rtldavis](https://github.com/bemasher/rtldavis)** — the definitive US 915 MHz frequency-hopping table and timing.
- **[dcbo/ISS-MQTT-Gateway](https://github.com/dcbo/ISS-MQTT-Gateway)** — the packet field-decoding formulas.

## License

MIT — see [LICENSE](LICENSE). Built by Dan Zubey (N7NMD).
