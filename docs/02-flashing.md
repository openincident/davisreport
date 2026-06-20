# 2. Loading the program onto the board ("flashing")

"Flashing" just means copying our program onto the board. We use a free tool called **PlatformIO** that handles all the messy parts (downloading the right compiler, the libraries, etc.) for you.

You only do steps 1–3 once. After that, flashing is a single command.

## Step 1: Install PlatformIO

PlatformIO runs on top of Python. If you have Python installed, open a terminal and run:

```bash
pip install platformio
```

That gives you a `pio` command. Check it worked:

```bash
pio --version
```

*(Prefer a graphical option? PlatformIO is also available as an extension inside the free VS Code editor. Either way works; the commands below assume the terminal.)*

## Step 2: Get the code and create your settings file

```bash
# Download this project (or use the folder you already have)
cd davisreport

# Make your personal settings file from the template
cp src/config.h.example src/config.h
```

Now open `src/config.h` in any text editor and fill in:

- **WiFi name and password** — so the board can reach your network.
- **MQTT host, username, password** — your Home Assistant / Mosquitto details (see [docs/04-home-assistant.md](04-home-assistant.md) if you don't have these yet).
- **STATION_ID** — your Davis "Station number" **minus 1** (Station 1 → `0`). This is important; it controls the exact timing the receiver expects.
- **USE_IMPERIAL_UNITS** — `1` for °F/mph/inches, `0` for °C/km/h/mm.

Save the file. (This file stays private on your computer — it's deliberately kept out of the public repository because it has your passwords in it.)

## Step 3: Plug in and flash

1. Attach the **antenna** to the board (do this first — see [docs/01](01-hardware-and-wiring.md)).
2. Plug the board into your computer with the **USB cable**.
3. Run:

```bash
pio run --target upload
```

The first time, this downloads the compiler and libraries — it can take a few minutes. Later flashes are quick. When it finishes you'll see a success message.

> **If the upload fails or hangs:** some boards need you to hold the **BOOT** button while it starts uploading, then release it. You can also try lowering `upload_speed` in `platformio.ini` to `460800`.

## Step 4: Watch it run

```bash
pio device monitor
```

This shows the board's live debug messages. A healthy startup looks roughly like:

```
=== davisreport: Davis weather receiver starting ===
[radio] started OK; searching for the station on channel 0...
[wifi] connecting to YourWiFiName
[mqtt] connected to broker.
[mqtt] sent Home Assistant discovery; sensors should appear now.
[radio] locked on! Now following the hop pattern.
```

The line **"locked on!"** is the one you're waiting for — it means the board found your weather station's signal. It can take up to about **two minutes** to lock on the first time (the board has to wait for the station's hopping pattern to come around to it). After that, readings should appear on the OLED screen and in Home Assistant.

Press `Ctrl+C` to quit the monitor. To unplug from the computer and run it permanently, just power the board from any USB charger.

Stuck? Head to [docs/05-troubleshooting.md](05-troubleshooting.md).
