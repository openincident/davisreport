# 3. How it works (the friendly explanation)

You don't need to read this to use the project — but if you're curious how a $15 board can eavesdrop on a weather station, here's the whole story in plain English.

## The setup

Your Davis weather station has two parts:

1. The **outdoor sensors** — Davis calls this bundle the **ISS** (Integrated Sensor Suite). It measures temperature, humidity, wind, rain, and (depending on your model) sunlight.
2. The **indoor console** — the little display unit that shows you the weather.

The outdoor sensors broadcast their readings over the air by radio, and the console listens. **There's no encryption and no pairing secret** — the sensors just shout their readings into the air for anyone tuned to the right frequency. Our board is simply a second listener. It doesn't interfere with your console at all; both can listen at once.

## Why we can't just "tune to one channel"

If the sensors always used a single frequency, this would be easy. But to be reliable (and to follow radio regulations), the Davis sensors use a trick called **frequency hopping**.

Picture 51 numbered radio channels. Every ~2.5 seconds the sensors send one short message, but each message goes out on a **different** channel, cycling through all 51 in a **fixed, repeating order**. Channel 0, then 19, then 41, then 25... and so on, eventually looping back to the start.

The order is the same for every Davis station in the world — it was figured out years ago by hobbyists (see the credits in the README). The console knows this order, so it follows along, hopping channels in perfect step with the sensors so it's always tuned to the right channel for the next message.

Our board has to do the exact same dance.

## How our board catches up: "searching" then "tracking"

When the board first powers on, it has no idea where the sensors currently are in their 51-channel cycle. So it does something clever and simple:

- **Searching:** It parks itself on channel 0 and just waits. Since the sensors cycle through all 51 channels, within about two minutes they'll land on channel 0 — and the board catches a message. Because channel 0 is the **first** channel in the known pattern, the board now knows *exactly* where the sensors are in the cycle.

- **Tracking:** From that moment on, the board hops along with the sensors. After catching each message, it immediately tunes to the **next** channel in the pattern, arriving there before the sensors transmit again. It catches every message this way.

- **Recovering:** Sometimes a message gets lost (interference, someone microwaving popcorn, whatever). The board tolerates a few misses by hopping ahead on schedule anyway, assuming the sensors are still ticking along. But if it misses too many in a row, it gives up trying to follow blindly, parks back on channel 0, and re-finds the signal. (You'll see this in the debug log if it happens.)

This logic is in `src/davis_radio.cpp`, with comments throughout.

## Making sure a message isn't garbled

Radio is noisy, so some received messages are corrupt. Every Davis message includes a small **checksum** — a number computed from the message's contents. The board redoes that same computation on what it received and compares. If they match, the message is intact; if not, it's thrown away. This check is in `src/davis_protocol.cpp` (the `davisCrcValid` function).

## Reading the weather out of the message

Each message is just **10 bytes** (80 ones-and-zeros). Here's the surprising part: a single message does **not** contain every reading. Every message always carries the **wind speed and direction**, but it only includes **one** of the other readings — temperature, *or* humidity, *or* rain, *or* sunlight — and the sensors rotate through them.

So the board keeps a running scoreboard (the `DavisData` structure in the code). Each message updates the wind, plus whichever single other reading it happened to carry. Over a handful of messages — a few seconds — every value gets refreshed. The decoding math (e.g. "temperature = these two bytes divided by 160") lives in `src/davis_protocol.cpp`, with each formula explained.

## Showing and sharing the readings

Once the board has current readings, two things happen:

1. **The OLED screen** (`src/davis_display.cpp`) is redrawn about twice a second with the latest numbers and a little health indicator.
2. **Home Assistant** (`src/davis_mqtt.cpp`) gets the readings over WiFi using a messaging system called **MQTT**. The board even tells Home Assistant how to set itself up, so the sensors appear automatically. That's covered in [docs/04-home-assistant.md](04-home-assistant.md).

And that's the whole magic trick: listen on the right channel at the right moment, check the message is clean, pull out the numbers, and pass them along.
