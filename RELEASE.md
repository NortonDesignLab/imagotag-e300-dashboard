# v1.0 — Nine-screen e-paper dashboard on a 7.5" tri-colour panel

A battery-free, always-on information panel built on a Waveshare e-Paper ESP32 Driver Board and a
salvaged 880×528 black/white/red e-paper display. It pulls ten public data sources directly over
Wi-Fi — no server, no broker, no cloud account — and rotates through nine screens. Ships with a
parametric 3D-printed snap-together frame that houses the driver board.

![the panel showing live weather](docs/weather.jpg)

---

## What it shows

| Screen | Contents |
|---|---|
| **Weather** | Live station: temperature with today's high/low and their times, dynamic FEELS LIKE / WIND CHILL / HEAT INDEX label, pressure with 3-hour trend, wind direction and max gust, UV, solar, sunrise/sunset/daylight, indoor conditions, rain by hour/day/week/month/year, and a lightning line that turns red when strikes occur within the hour |
| **Forecast** | NWS: next five periods with temperature, conditions, precipitation chance and wind, plus the full narrative for the current period |
| **Air & Allergy** | Pollen index with named allergens, asthma index, US AQI broken out into PM2.5 / PM10 / ozone, and a five-day pollen forecast |
| **River** | USGS gauge: stage, streamflow, 24-hour trend chart |
| **Space Weather** | Planetary Kp with storm banding, a three-day history chart, and an aurora-visibility line calibrated to the site's latitude |
| **Earthquakes** | US events, M2.5+ in the past 24 hours, **sorted nearest-first** by great-circle distance |
| **Portfolio** | Holdings with day and total gain/loss, market open/closed indicator, and a market-context strip (S&P, Nasdaq, VIX, 10-year yield, gold) |
| **Electricity** | ComEd hourly pricing: current hour average, day low/high/avg, and a 24-hour bar chart with an 8¢ alert threshold |
| **News** | Wall Street Journal headlines with relative ages |

Weather takes **every third rotation slot** — it is never more than nine minutes away, while the
other eight cycle past in turn.

## Severe weather takeover

A tornado or severe thunderstorm warning for the exact coordinates draws a red-bordered box over
the weather screen with the event, the affected area and the expiry time. It **repaints
immediately** rather than waiting for the next rotation, **pins the display** so it cannot scroll
away, and **outranks any user message**. Tornado warnings get a heavier frame than thunderstorm
warnings. A `/testalert` endpoint exercises the whole path without waiting for a storm.

## Messages over Wi-Fi

Any browser on the LAN can push a message to the panel — a banner across the bottom or a
full-screen takeover, with an optional expiry after which the dashboard returns on its own.
Wrapping words in asterisks renders them in red:

```bash
curl -X POST http://<board-ip>/message \
  --data-urlencode "text=Dinner at *6 pm* - bring the *dog food*" -d style=banner -d ttl=120
```

## Data sources

Ten endpoints, all called straight from the ESP32:

`rt.ambientweather.net` · `api.weather.gov` · `pollen.com` · `air-quality-api.open-meteo.com` ·
`waterservices.usgs.gov` · `earthquake.usgs.gov` · `services.swpc.noaa.gov` ·
`query1.finance.yahoo.com` · `hourlypricing.comed.com` · `news.google.com`

Only Ambient Weather needs credentials, and those are your own station's. Everything else is
keyless. Polling is staggered by volatility: severe alerts every 2 minutes, electricity every 5,
weather and quotes every 10, river and quakes every 15, forecast, air and news every 30.

## The frame

`frame/make_frame.py` is parametric — change a constant, re-run, get new STLs. Three printed parts,
**no fasteners**: barbed posts snap the back plate to the bezel, and the same geometry snaps a lid
onto the driver-board enclosure moulded into the back. The enclosure's floor is the back plate
itself, so the display ribbon passes straight up through it with no external routing.

---

## Notes from the build

Things that cost real time and are worth knowing if you build one:

- **The panel is not what its part number suggests.** A ribbon marked `HINK-E075A07-A0` is a
  **GDEH075Z90 — 880×528 tri-colour on an SSD1677**, not the 800×480 UC8179 the "7.5 inch" naming
  implies. Driven as the latter it reports a successful refresh in ~1 ms and draws nothing, which
  sends you hunting a wiring fault that does not exist.
- **WSJ's RSS feeds are dead** — frozen since January 2025, with the origin's `last-modified` header
  to prove it is the source and not a cache. Build against them and you get a screen confidently
  displaying twenty-month-old news.
- **ESP32's newlib-nano printf silently drops 64-bit format specifiers.** Epoch milliseconds
  needs 13 digits; `%lld` produces a malformed URL and no error.
- **`struct tm` has no `tm_gmtoff`** on ESP32, so UTC offset has to be derived by comparing local
  and UTC breakdowns — including the day and month wrap.
- **Timestamp formats differ per source.** NWS stamps carry a local UTC offset (`-06:00`); Ambient
  Weather uses `Z`. Assuming UTC everywhere puts alert expiry times hours out.
- **ComEd's daily feed is JavaScript, not JSON** (`[[Date.UTC(…), price], …]`), and despite the
  function name those hours are **local clock hours**.
- **GFX fonts are ASCII-only.** Headlines full of curly quotes and en-dashes render as blanks
  unless transliterated.
- **Lay dense rows out by measurement, not assumed character widths.** Fixed columns overlapped in
  practice because the real font was ~10 px/char, not the 8 estimated.

## Requirements

Waveshare e-Paper ESP32 Driver Board (ESP32-WROOM-32E) · GDEH075Z90 880×528 tri-colour panel ·
`arduino-cli` with esp32 core 3.3.11 · GxEPD2 1.6.9 · ArduinoJson 7.4.3 · 2.4 GHz Wi-Fi

Build with `PartitionScheme=huge_app` — the default 1.3 MB app partition runs out around the
fourth screen. Current build sits at 40% of flash and 36% of RAM, so there is room for more.

## Roadmap

- Web/NVS-editable portfolio holdings, so trades do not need a reflash
- Home lab health screen (service up/down, GPU temps) off an existing Prometheus
- Flight tracker fed by a local ADS-B receiver
- Four-colour sibling: see **[docs/E300-7.4-BWRY.md](docs/E300-7.4-BWRY.md)** for driving a surplus
  **Imagotag E300 7.4" BWRY**
  (Pervasive Displays E2741QS0B3, 800×480 black/white/red/yellow) via `bb_epaper`, and
  `NDLPanel/` is a branded message panel on the same hardware. These turn up cheaply on eBay.
