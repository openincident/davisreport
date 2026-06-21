# 6. The onboard web dashboard, history & metrics

Besides the OLED and Home Assistant, the board runs its own little website and a
metrics feed. This covers what's there and how to use it.

## Opening it

With the board on your WiFi, browse to **http://davis.local** (or its IP — the
serial log prints both at startup). You get:

- **Live cards** — temperature, dew point, humidity, wind (with a rotating
  compass), gust, rain, and a red banner when a SkyWarn alert is active.
- **A wind-direction compass** with a weather-vane needle.
- **Live charts** (last ~24 h, 2-minute resolution) — temperature, dew point,
  humidity, wind & gust, rain. Auto-ranged to the data's 5th–95th percentile so
  outliers don't flatten them.
- **Long-term charts** with a **7 / 30 / 90 / 180-day** selector — temperature
  and humidity as an average line with a min/max band, wind average + gust max,
  and rain per hour.

The board learns the real time over **NTP** at startup (set your timezone with
`NTP_TZ` in `config.h` — Arizona is `MST7`), so all the timestamps are real
local times.

## How history is stored (and what survives a reboot)

| What | Resolution | Span | Survives reboot? |
|------|-----------|------|------------------|
| Live ring | 2 min | ~24 h | **Yes** — snapshotted to flash every 10 min |
| Long-term store | 1 hour (min/avg/max) | **180 days** | **Yes** — it lives in flash |

Both are kept in the board's flash (LittleFS) on the spare data partition. The
long-term store is a ~82 KB circular file that writes one record per hour, so
flash wear is negligible (a sector erase about once a week — it'll outlast the
board). After a reboot or crash, the charts pick up where they left off.

> Long-term history accumulates as the board runs — a fresh board shows only
> what it has collected so far, filling out to 180 days over time.

## URLs the board serves

| URL | What it returns |
|-----|-----------------|
| `/` | the dashboard page |
| `/data.json` | current conditions + the live (24 h) history |
| `/longterm.json?days=N` | the hourly long-term archive for the last N days |
| `/metrics` | Prometheus metrics (see below) |
| `/clear?confirm=yes` | **erases** all stored history (live + long-term) |

## Long-term / remote: Prometheus → Grafana

`/metrics` exposes everything in Prometheus format (all names prefixed
`davis_`). Point a scraper at it for unlimited, durable history and Grafana
dashboards. A ready-made **Grafana Alloy** config is in
[../monitoring/alloy-davis.alloy](../monitoring/alloy-davis.alloy) — fill in your
Grafana Cloud push URL + credentials and the board's address, and metrics flow
to Grafana Cloud. (Home Assistant also keeps long-term statistics on its own, so
between HA, Grafana, and the on-board store you have three independent archives.)

## Tip: give the board a fixed address

The board's DHCP IP can change when it reboots. For reliable scraping (and so
you don't have to chase the IP), set a **DHCP reservation** for the board's MAC
in your router, or use `davis.local` where mDNS resolves. This matters most for
the Alloy scrape target.

## Turning things off

In `config.h`: `WEB_ENABLE 0` disables the whole web server. `WEB_SAMPLE_SECONDS`
and `WEB_HISTORY_POINTS` size the live ring. The long-term store is always on
when the web server is.
