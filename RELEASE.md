# v1.0 — A nine-screen information panel built from a retired electronic shelf label

Retailers replace **SES-imagotag / Vusion** shelf labels in bulk, and the retired ones sell for very
little on eBay. Inside a **7.4" E300** is a genuinely good four-colour e-paper panel — a Pervasive
Displays **E2741QS0B3** "Spectra 4", 800×480, black/white/red/yellow — that would otherwise be
e-waste. It needs no adapter and no level shifting: it plugs straight into a stock Waveshare e-Paper
ESP32 driver board and runs from an ESP32-WROOM-32.

This repo turns one into an always-on dashboard that pulls **ten public data sources directly over
Wi-Fi** — no server, no broker, no cloud account, nothing running on a PC — and rotates through nine
screens. It ships with a parametric 3D-printed snap-together frame.

**Start here: [docs/E300-7.4-BWRY.md](docs/E300-7.4-BWRY.md)** — wiring, a minimal working sketch,
and the four things that cost real time.

---

## What it shows

`WeatherPanel4C/` is the four-colour build. Nine screens, rotating every 3 minutes, with live
weather taking every third slot so it is never more than nine minutes away:

| Screen | Contents |
|---|---|
| **Weather** | Live station: temperature with today's high/low, a label that switches between FEELS LIKE / WIND CHILL / HEAT INDEX, pressure with 3-hour trend, wind direction and max gust, UV, solar, sunrise/sunset/daylight, indoor conditions, rain by hour/day/week/month/year, and a lightning line |
| **Forecast** | NWS: next five periods plus the full narrative for the current one |
| **Air & Allergy** | Pollen index with named allergens, asthma index, AQI broken into PM2.5 / PM10 / ozone, five-day pollen outlook |
| **River** | USGS gauge: stage, streamflow, 24-hour trend |
| **Space Weather** | Planetary Kp with storm banding and a three-day chart on a time-labelled axis |
| **Earthquakes** | US events, M2.5+ in 24 h, sorted **nearest-first** by great-circle distance |
| **Portfolio** | Holdings with day and total gain/loss, market open/closed, and an index strip (S&P, Nasdaq, VIX, 10-year, gold) |
| **Electricity** | ComEd hourly pricing with a 24-hour chart and an 8¢ alert line |
| **News** | Headlines with relative ages |

The fourth colour earns its place as a **caution tier between black and red** — UV 3–5, pollen or
asthma in the Medium band, AQI 51–100, Kp 4, M5 quakes, lightning earlier today but not within the
hour, a closed market (prices are not live), and the electricity hour still in progress.

## Severe weather takes over

A tornado or severe thunderstorm warning for your exact coordinates draws a bordered box over the
weather screen with the event, area and expiry. It repaints **immediately** rather than waiting for
the rotation, **pins** the display so it cannot scroll away, and **outranks any user message**. A
`/testalert` endpoint exercises the whole path without waiting for a storm.

## Messages over Wi-Fi

Any browser on the LAN can push a banner or a full-screen message, with an optional expiry after
which the dashboard returns by itself:

```bash
curl -X POST http://<board-ip>/message \
  --data-urlencode "text=Dinner at *6 pm* - bring the *dog food*" -d style=banner -d ttl=120
```

## Data sources

`rt.ambientweather.net` · `api.weather.gov` · `pollen.com` · `air-quality-api.open-meteo.com` ·
`waterservices.usgs.gov` · `earthquake.usgs.gov` · `services.swpc.noaa.gov` ·
`query1.finance.yahoo.com` · `hourlypricing.comed.com` · `news.google.com`

Only Ambient Weather needs credentials, and those are your own station's — the other eight screens
work with no account anywhere. Polling is staggered by volatility: severe alerts every 2 minutes,
electricity every 5, weather and quotes every 10, river and quakes every 15, the rest every 30.

## What else is in here

| | |
|---|---|
| `WeatherPanel4C/` | The four-colour build on the E300 ESL panel |
| `Imagotag74Test/` | Smallest possible bring-up for the E300 — colour bars. Start here |
| `NDLPanel/` | Branded message panel on the same hardware |
| `WeatherEpaper/` | Tri-colour sibling on a GDEH075Z90 880×528 panel, via GxEPD2 |
| `frame/` | Parametric snap-together 3-part frame with a driver-board enclosure |

Credentials, station name **and portfolio positions** all live in `secrets.h`, which is gitignored.
Copy `secrets.h.example` — it ships with a demo portfolio of five index ETFs so the Portfolio screen
works immediately.

---

## Notes from the build

The parts that cost hours and are not findable by searching:

- **A ribbon marked `HINK-E075A07-A0` is not what the name implies.** It is a GDEH075Z90 — 880×528
  tri-colour on an SSD1677, not the 800×480 UC8179 that "7.5 inch" suggests. Driven as the latter it
  reports a successful refresh in ~1 ms and draws nothing, sending you after a wiring fault that does
  not exist.
- **The E300 needs its init sequence re-sent before every frame** (`BBEP_NEEDS_EXTRA_INIT`), and
  `bb_epaper`'s 96 KB full-frame buffer starves TLS on a no-PSRAM WROOM-32 — render in bands and
  stream instead. Both are covered in the E300 guide.
- **WSJ's RSS feeds are dead**, frozen since January 2025, with the origin's `last-modified` header
  to prove it is the source and not a cache. Build against them and you ship twenty-month-old news.
- **ESP32's newlib-nano printf silently drops 64-bit format specifiers.** Epoch milliseconds needs
  13 digits; `%lld` yields a malformed URL and no error.
- **`struct tm` has no `tm_gmtoff`** here, so the UTC offset must be derived by comparing local and
  UTC breakdowns — including the day and month wrap.
- **Timestamp formats differ per source.** NWS carries a local offset (`-06:00`); Ambient Weather
  uses `Z`. Assume UTC everywhere and alert expiry times land hours out.
- **ComEd's daily feed is JavaScript, not JSON**, and despite the `Date.UTC(...)` wrapper those hours
  are local clock hours.
- **GFX fonts are ASCII-only** — headlines full of curly quotes render as blanks unless
  transliterated.
- **Lay dense rows out by measurement, not assumed character widths.** Fixed columns overlapped in
  practice because the real font was ~10 px/char, not the 8 estimated.
- **Never use `//` inside a line-continued macro.** The preprocessor splices the continuations into
  one logical line *before* stripping comments, so one `//` silently swallows everything after it.
  It still compiles — it cost six of seven portfolio holdings before the symptom surfaced.

## Requirements

Waveshare e-Paper ESP32 Driver Board (Rev3) · an E300 7.4" BWRY panel (E2741QS0B3) ·
`arduino-cli` with esp32 core 3.3.11 · **`bb_epaper` from git master** (for `EP74YR_480x800`) ·
Adafruit GFX · ArduinoJson 7.4.3 · 2.4 GHz Wi-Fi

Build with `PartitionScheme=huge_app`; the default 1.3 MB app partition runs out around the fourth
screen. Current build is 41% of flash and 33% of RAM.

## Roadmap

- Web/NVS-editable portfolio holdings, so trades do not need a reflash
- Home lab health screen (service up/down, GPU temps) off an existing Prometheus
- Flight tracker fed by a local ADS-B receiver
