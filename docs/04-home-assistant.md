# 4. Home Assistant setup

The board sends its readings to Home Assistant using **MQTT**, a simple messaging system. The best part: the board describes its own sensors, so Home Assistant creates them **automatically**. You don't have to write any sensor YAML.

Here's how to make sure the pieces are in place.

## What MQTT is (in one paragraph)

MQTT is like a post office. The board "publishes" (drops off) messages with the weather readings. Home Assistant "subscribes" (picks up) those messages. In the middle sits a **broker** — the actual post office — which on Home Assistant is almost always the **Mosquitto** add-on. So you need: a broker running, and a username/password the board can use to log into it.

## Step 1: Make sure you have an MQTT broker

If you already use any MQTT devices with Home Assistant, you have this and can skip ahead.

Otherwise, install the broker:

1. In Home Assistant, go to **Settings → Add-ons → Add-on Store**.
2. Find **Mosquitto broker** and install it.
3. Start it, and turn on **"Start on boot."**

## Step 2: Create a login for the board

The board needs a username and password to talk to the broker. The simplest way:

1. Go to **Settings → People → Users** (or **Settings → Devices & Services → Users** depending on your version).
2. Add a new user, e.g. username `mqtt` with a password you choose. (A normal Home Assistant user works as an MQTT login with the Mosquitto add-on's default settings.)
3. Put that username and password into your `src/config.h` as `MQTT_USER` and `MQTT_PASSWORD`.

For `MQTT_HOST`, use your Home Assistant's IP address (e.g. `192.168.1.50`). `MQTT_PORT` stays `1883`.

## Step 3: Make sure the MQTT integration is set up

1. Go to **Settings → Devices & Services**.
2. If **MQTT** isn't already listed, click **Add Integration**, search **MQTT**, and point it at your broker (usually it auto-detects `core-mosquitto`).

This integration is what listens for the board's "create my sensors" messages.

## Step 4: Flash the board and watch them appear

Once the board is flashed ([docs/02](02-flashing.md)) and connects, it sends Home Assistant a set of "discovery" messages. Within a few seconds you should see a new device:

- Go to **Settings → Devices & Services → MQTT**.
- You'll find a device named **Davis Weather Station** with sensors for temperature, humidity, wind speed, wind gust, wind direction, rain rate, rain total, and solar radiation — plus a few diagnostics (signal strength, supercap voltage, battery-low).

If they don't appear, see [docs/05-troubleshooting.md](05-troubleshooting.md).

## Step 5 (optional): Add the dashboard

This repo includes a ready-made dashboard card in **[../homeassistant/dashboard.yaml](../homeassistant/dashboard.yaml)**.

To use it:

1. Open any Home Assistant dashboard and click the pencil (edit) → the three dots → **Edit in YAML** (or add a new **Manual** card).
2. Paste the contents of `dashboard.yaml`.
3. If you changed `DEVICE_ID` in `config.h` away from the default `davis_iss`, do a find-and-replace on the entity names accordingly (they look like `sensor.davis_weather_station_temperature`).

That gives you a tidy weather panel with all the readings in one place.

## How the readings flow (for reference)

```
board ── publishes ──▶  topic: davis_iss/state   (a small JSON message)
                        topic: davis_iss/status  ("online" / "offline")
board ── publishes ──▶  topic: homeassistant/sensor/.../config  (one-time "create me")

Home Assistant ── reads the config topics ──▶ creates the sensors
Home Assistant ── reads davis_iss/state   ──▶ updates the sensor values
```

You can watch these raw messages yourself under **Settings → Devices & Services → MQTT → Configure → Listen to a topic**, and enter `davis_iss/#` to see everything the board sends.
