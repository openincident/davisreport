# 1. Hardware and wiring

Good news first: **there is almost no wiring to do.** The radio and the screen are already built into the LilyGO board. You mostly just need the right board and an antenna.

## What you need

| Item | Notes |
|------|-------|
| **LilyGO T3-S3 board — 915 MHz version (SX1262)** | This is the brain + radio + screen, all in one. **The frequency matters — see below.** |
| **915 MHz antenna** | Usually included with the board. Must match the board's frequency. |
| **USB cable** | To program the board and to power it. A normal phone-style cable. |
| **A USB power supply** | Any phone charger works for running it day-to-day. |
| *(optional)* A small case | The board is bare; a case keeps dust off. |

## ⚠️ The single most important thing: get the 915 MHz version

Davis weather stations sold in the **United States** broadcast on the **915 MHz** band. The LilyGO T3-S3 is sold in several look-alike versions — **433 MHz**, **868 MHz**, **915 MHz**, and a **2.4 GHz** one — and they are physically almost identical. If you get the wrong one, the board simply **won't hear** your weather station, no matter how perfect the software is.

**How to check which one you have:**

- **The listing/box** should say "915 MHz" or "US915."
- **The radio chip** on the board (printed on the little metal can): for 915 MHz it's a Semtech **SX1262** (this project's default) or sometimes an **SX1276**. Both are sub-GHz chips that this code can use — the SX1262 is what the standard T3-S3 ships with. Avoid the **SX1280** version: that's a **2.4 GHz** chip and **cannot** receive a 915 MHz Davis station at all.
- **The antenna** that came with it is often labeled with its frequency.
- When in doubt, ask the seller before buying.

*(If your board's can reads **SX1276** instead of SX1262, the firmware needs a one-line driver swap — open an issue or check the comments in `src/davis_radio.cpp`. If you're in **Europe**, your Davis is on **868 MHz**; you'd use the 868 MHz board and swap the frequency table in `src/davis_radio.cpp` for the European hop list. This repo ships with the US 915 MHz table and the SX1262 driver.)*

## The antenna

**Always attach the antenna before powering the board on.** Transmitting (which this project doesn't really do) without an antenna can damage the radio, and even for receiving you want it connected for any signal at all. Screw the included antenna onto the gold SMA connector — finger-tight is enough.

## Where to put it

- Anywhere it can **hear your outdoor sensors** — same as your Davis console's range, roughly. A line of sight or a few interior walls is usually fine.
- Near a **USB power outlet**.
- It needs **WiFi coverage** to reach Home Assistant.

A spot reasonably central in the house, not buried in a metal cabinet, is ideal.

## About the pins (only if something doesn't work)

The software needs to know which internal connections (pins) the radio and screen use. Those are already filled in for the T3-S3 in `src/config.h` (you copy it from `src/config.h.example`). You should not need to touch them.

If the board reports the radio failed to start (you'll see it in the serial monitor — see [docs/02-flashing.md](02-flashing.md)), your board revision may use slightly different pins. The values to double-check live in the "Hardware pin map" section of `config.h`, and your board's product page or schematic will list the correct ones.
