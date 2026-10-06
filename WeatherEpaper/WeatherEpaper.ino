// Ambient Weather display for the Waveshare e-Paper ESP32 Driver Board (Rev3, ESP32-WROOM-32E)
// driving a 7.5" HINK-E075A07-A0 = GDEH075Z90 880x528 black/white/RED (SSD1677) panel.
//
// Port of Bradley's WeatherDisplay_ideaspark (WROOM-32 + TFT_eSPI) to GxEPD2 3-color e-paper.
// The AWN fetch/parse core is reused verbatim, including the two WROOM-32-specific fixes:
//   * WPA2/WPA3-mixed retry loop (disconnect/begin every ~8 s until connected)
//   * static 48 KB body buffer (no PSRAM; malloc(120K) fails once TLS is up)
//
// Polls the AWN API every 10 min. A full refresh on this panel takes ~23 s, so the screen is
// only redrawn when something it shows has actually changed (new observation, changed value);
// otherwise the poll is silent. Red plane: falling pressure, freeze / heat temps, fresh lightning.
//
// Pins (board-fixed): BUSY=25 RST=26 DC=27 CS=15 CLK=13 DIN=14. SPI remap after display.init().

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <GxEPD2_3C.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include "secrets.h"
#include <WebServer.h>
#include <ESPmDNS.h>

// ---------------- message-over-Wi-Fi ----------------
// POST /message  text=...&style=banner|full&ttl=<minutes, 0 = until cleared>
// POST /clear    ;  GET / = browser form.  Host: http://epaper.local  (mDNS) or the board IP.
WebServer server(80);
String   msgText;                  // empty = no message
bool     msgFull    = false;       // full screen vs banner strip
uint32_t msgSetMs   = 0, msgTtlMs = 0;
bool     needRender = false;

// ---------------- portfolio ----------------
// Holdings, cash and treasury come from secrets.h (gitignored) so personal positions stay
// out of the repo - see secrets.h.example for the format. Prices are fetched live from Yahoo.
struct Holding { const char* sym; float qty; float cost; float price; float prev; time_t at; };
Holding HOLD[] = { PORTFOLIO_HOLDINGS };
const int NHOLD = sizeof(HOLD)/sizeof(HOLD[0]);
float CASH_USD     = PORTFOLIO_CASH;       // settlement / sweep balance
// Listed at $1.00/unit with zero gain, and its "percent of account" reads 100%, so it sits in a
// separate account. Shown as its own row but EXCLUDED from the total (Bradley, 29 Sep 2026).
float TREASURY_USD = PORTFOLIO_TREASURY;   // separate account: shown as a row, excluded from the total
// Market context shown under the holdings. '^' must be %5E-encoded in the URL.
struct Idx { const char* label; const char* path; int dec; bool pctUnit; float price; float prev; };
Idx IDX[] = {
  {"S&P",  "%5EGSPC", 0, false, 0, 0},
  {"NASD", "%5EIXIC", 0, false, 0, 0},
  {"VIX",  "%5EVIX",  1, false, 0, 0},
  {"10YR", "%5ETNX",  2, true,  0, 0},   // ^TNX is already a yield in percent
  {"GOLD", "GC=F",    0, false, 0, 0},   // COMEX gold future
};
const int NIDX = sizeof(IDX)/sizeof(IDX[0]);
int idxOk = 0;

const uint32_t QUOTES_MS = 10UL * 60 * 1000;
uint32_t lastQuotes = 0;
int      quotesOk = 0;
time_t   quoteAt = 0;

// ---------------- ComEd hourly pricing ----------------
// northern Illinois. Free, no key. Two calls:
//   ?type=currenthouraverage -> JSON [{millisUTC,price}]  (45 B)
//   ?type=day&date=YYYYMMDD  -> JavaScript, NOT JSON: [[Date.UTC(y,m,d,H,0,0), price], ...]  (~550 B)
// The Date.UTC() hours are LOCAL clock hours despite the name (verified: 16 entries at 16:07 CDT),
// so H indexes straight into a local 0..23 array with no timezone maths.
const float ALERT_C = 8.0f;              // cents/kWh: at or above this is drawn red
float  comedHour[24];
float  comedCur = NAN;
int    comedN = 0;
time_t comedAt = 0;
const uint32_t COMED_MS = 5UL * 60 * 1000;
uint32_t lastComed = 0;

// ---------------- NWS forecast ----------------
// Free, no key, no special User-Agent needed. The gridpoint is fixed for a location - it came
// from GET /points/41.8781,-87.6298 -> gridId LOT, gridX 65, gridY 95 (Chicago, IL). Re-derive
// from /points if SITE_LAT/SITE_LON change. Body is ~13.6 KB, well inside BODY_CAP.
const char* NWS_URL = "https://api.weather.gov/gridpoints/LOT/76,73/forecast";
struct Period { char name[20]; char shortF[48]; char wind[24]; int temp; int pop; bool day; };
Period  fc[5];
int     fcN = 0;
char    fcDetail[320] = "";
time_t  fcAt = 0;
const uint32_t FC_MS = 30UL * 60 * 1000;    // NWS refreshes about hourly
uint32_t lastFc = 0;

// ---------------- pollen + air quality ----------------
// Pollen: pollen.com by ZIP, free/keyless but REQUIRES a Referer header or it returns nothing.
//   Index is 0-12 and it names the active triggers. Its /mold/ endpoint returns null for 60606 -
//   there is effectively no mold data for this area (NAB counts have no public API).
// AQI: Open-Meteo air-quality, free/keyless. NOTE its pollen fields are Europe-only and come back
//   null for US coordinates, which is why pollen comes from pollen.com instead.
const char* POLLEN_ZIP = "60606";
float  pollenIdx = NAN;
char   pollenTrig[72] = "";
int    aqiVal = -1;
float  pm25 = NAN;
float  asthmaIdx = NAN;
float  pm10 = NAN, ozone = NAN;
float  pollen5[5];
int    p5mon[5], p5day[5], pollen5N = 0;
time_t airAt = 0;
const uint32_t AIR_MS = 30UL * 60 * 1000;
uint32_t lastAir = 0;

// ---------------- NWS severe weather alerts ----------------
// api.weather.gov/alerts/active?point=lat,lon - free, keyless, tiny payload (196 B when clear).
// Only Tornado / Severe Thunderstorm WARNINGS raise the box (watches and advisories do not).
bool   alertActive = false;
char   alertEvent[48] = "";
char   alertArea[72]  = "";
time_t alertEnds = 0;
int    alertRank = 0;                        // 2 = tornado, 1 = thunderstorm
const uint32_t ALERT_MS = 2UL * 60 * 1000;   // time-critical: poll every 2 min
uint32_t lastAlert = 0;
bool     alertTest = false;      // /testalert holds the box up without a real storm

// ---------------- WSJ headlines ----------------
// WSJ's own RSS (feeds.a.dj.com) is DEAD - frozen since 27 Jan 2025 (origin last-modified proves
// it, and a cache-buster changes nothing). Google News filtered to site:wsj.com is live. The feed
// is ~120 KB but the top 8 items land inside 11 KB, so we read ~16 KB and hang up.
const char* NEWS_URL =
  "https://news.google.com/rss/search?q=site:wsj.com+when:2d&hl=en-US&gl=US&ceid=US:en";
struct Story { char title[124]; time_t t; };
const int NEWS_MAX = 8;
Story  NEWS[NEWS_MAX];
int    newsN = 0;
time_t newsAt = 0;
const uint32_t NEWS_MS = 30UL * 60 * 1000;
uint32_t lastNews = 0;

// ---------------- river gauge (USGS) ----------------
// "Des Plaines River near Chicago, IL", USGS site 05533600. Keyless, 15-minute cadence.
// 00065 = gage height (ft), 00060 = streamflow (ft3/s).
// FLOOD STAGES ARE DELIBERATELY UNSET. NWS publishes them per forecast point, but none of the
// candidate NWPS gauge IDs resolved, and guessing a flood threshold on a flood display is worse
// than showing none. Fill these in from the NWS AHPS page for this gauge and the marker appears.
const char* USGS_SITE = "05533600";
const float FLOOD_ACTION = 6.5f;     // ft - NWS LEMI2 (Bradley, 6 Oct 2026); not yet reflashed
const float FLOOD_MINOR  = 7.0f;     // ft - moderate 9.0, major 11.0
float  rivStage = NAN, rivFlow = NAN, rivFirst = NAN;
float  rivHist[48]; int rivHistN = 0;
time_t rivAt = 0;
const uint32_t RIVER_MS = 15UL * 60 * 1000;
uint32_t lastRiver = 0;

// ---------------- earthquakes (USGS) ----------------
struct Quake { char place[58]; float mag; float dist; time_t t; };   // dist = miles from SITE
const int QK_MAX = 7;
Quake  QK[QK_MAX]; int qkN = 0, qkTotal = 0; float qkBig = 0; time_t qkAt = 0;
const uint32_t QUAKE_MS = 15UL * 60 * 1000;
uint32_t lastQuake = 0;

// ---------------- space weather (NOAA SWPC) ----------------
float  kpNow = NAN; float kpHist[24]; time_t kpTime[24]; int kpHistN = 0; time_t kpAt = 0;
const uint32_t SPACE_MS = 30UL * 60 * 1000;
uint32_t lastSpace = 0;

// ---------------- screen rotation ----------------
enum Screen { SCR_WEATHER = 0, SCR_FORECAST = 1, SCR_AIR = 2, SCR_RIVER = 3, SCR_SPACE = 4,
              SCR_QUAKE = 5, SCR_PORTFOLIO = 6, SCR_COMED = 7, SCR_NEWS = 8 };
const int NUM_SCREENS = 9;
// Station weather gets every 3rd slot; the other screens fill the gaps in order. Full cycle is
// 12 slots (8 others + 4 weather), so weather returns every 3 slots and the rest every 12.
const Screen OTHERS[] = { SCR_FORECAST, SCR_AIR, SCR_RIVER, SCR_SPACE,
                          SCR_QUAKE, SCR_PORTFOLIO, SCR_COMED, SCR_NEWS };
const int NOTHER = sizeof(OTHERS) / sizeof(OTHERS[0]);
int rotSlot = 0, otherIdx = 0;
Screen   curScreen = SCR_WEATHER;
uint32_t rotateMs  = 3UL * 60 * 1000;           // 0 = stay on the current screen
uint32_t lastRotate = 0;

// Paged at HEIGHT/8: the WROOM-32 has no PSRAM and the 48 KB body buffer + Wi-Fi/TLS statics
// leave too little DRAM for /4 paging (overflowed dram0_0_seg by ~4 KB).
GxEPD2_3C<GxEPD2_750c_Z90, GxEPD2_750c_Z90::HEIGHT / 8> display(
    GxEPD2_750c_Z90(/*CS=*/ 15, /*DC=*/ 27, /*RST=*/ 26, /*BUSY=*/ 25));

#define SCR_W 880
#define SCR_H 528

const float INHG2MB = 33.8639f;
const uint32_t CURRENT_MS = 10UL * 60 * 1000;   // poll current every 10 min (Bradley, 21 Sep 2026)
const uint32_t HISTORY_MS = 10UL * 60 * 1000;   // poll history every 10 min
static const int HIST_MAX   = 60;
static const int HIST_LIMIT = 48;               // 5-min samples -> 4 h (covers the 3 h trend)
const float TREND_MB = 0.7f;
const double SITE_LAT = 41.8781, SITE_LON = -87.6298;   // Chicago, IL - edit if the station moves

// ---------------- data ----------------
struct Obs {
  float tempf = NAN, humidity = NAN, feelsLike = NAN, dewPoint = NAN;
  float baromRel = NAN, baromAbs = NAN, windMph = NAN, windGust = NAN, dailyRain = NAN;
  float ltgDay = NAN, ltgHour = NAN, ltgDist = NAN; time_t ltgTime = 0;
  float windDir = NAN, windDirAvg = NAN, windAvg = NAN, maxGust = NAN;
  float uv = NAN, solar = NAN, tempIn = NAN, humIn = NAN;
  float rainHour = NAN, rainWeek = NAN, rainMonth = NAN, rainYear = NAN;
  time_t lastRainT = 0;
  time_t t = 0;
  bool ok = false;
};
Obs cur;
float  histP[HIST_MAX];
time_t histT[HIST_MAX];
int    histN = 0;
float  trendDelta = NAN;
int    trendSpanMin = 0;
String lastErr;
uint16_t fetchOk = 0, fetchTotal = 0;
time_t lastGood = 0;
// Today's high/low. Not in the AWN feed, so backfilled from midnight at boot by paging the
// history endpoint with endDate (48 recs / ~36 KB / 4 h per call), then tracked forward.
float  dayHi = NAN, dayLo = NAN;
time_t dayHiT = 0, dayLoT = 0;
int    dayKey = -1;                 // tm_yday the hi/lo belong to

// ---------------- helpers ----------------
static String fmtTime(time_t t, const char* fmt = "%l:%M %p") {
  if (t < 1000000000) return "--:--";     // 0 / pre-NTP values render as "6:00 PM" (epoch in Chicago)
  struct tm tm; localtime_r(&t, &tm);
  char buf[32]; strftime(buf, sizeof buf, fmt, &tm);
  String s(buf); s.trim(); return s;
}
static String f0(float v, const char* suffix = "") { char b[16]; if (isnan(v)) return "--"; snprintf(b, sizeof b, "%.0f%s", v, suffix); return String(b); }
static String f1(float v, const char* suffix = "") { char b[16]; if (isnan(v)) return "--"; snprintf(b, sizeof b, "%.1f%s", v, suffix); return String(b); }

// ---------------- Ambient Weather fetch (reused from ideaspark / C6) ----------------
static const char* KEEP[] = {"dateutc","tempf","humidity","feelsLike","dewPoint","baromrelin",
                             "baromabsin","windspeedmph","windgustmph","dailyrainin",
                             "lightning_day","lightning_hour","lightning_distance","lightning_time",
                             "winddir","winddir_avg10m","windspdmph_avg10m","maxdailygust",
                             "uv","solarradiation","tempinf","humidityin",
                             "hourlyrainin","weeklyrainin","monthlyrainin","yearlyrainin","lastRain"};
static const size_t BODY_CAP = 48000;
static char bodyBuf[BODY_CAP];
static bool httpGetRaw(const String& url, size_t& outLen, size_t maxBytes = BODY_CAP - 1,
                       const char* referer = nullptr) {
  uint32_t t0 = millis();
  WiFiClientSecure client; client.setInsecure();
  HTTPClient https;
  https.useHTTP10(true);
  https.setTimeout(15000);
  https.setUserAgent("Mozilla/5.0 (X11; Linux x86_64)");   // Yahoo 404s the default ESP32 UA
  if (!https.begin(client, url)) { lastErr = "begin failed"; return false; }
  if (referer) https.addHeader("Referer", referer);   // pollen.com returns nothing without it
  int code = https.GET();
  if (code != HTTP_CODE_OK) {
    lastErr = "HTTP " + String(code);
    if (code == 401) lastErr += " keys";
    if (code == 429) lastErr += " rate";
    Serial.printf("  GET %s FAILED: %s\n", url.substring(0, 45).c_str(), lastErr.c_str());
    https.end(); return false;
  }
  NetworkClient* st = https.getStreamPtr();
  size_t len = 0; uint32_t last = millis();
  if (maxBytes > BODY_CAP - 1) maxBytes = BODY_CAP - 1;
  while (millis() - last < 5000 && len < maxBytes) {
    int n = st->available();
    if (n > 0) {
      if ((size_t)n > maxBytes - len) n = maxBytes - len;
      n = st->read((uint8_t*)bodyBuf + len, n);
      if (n > 0) { len += n; last = millis(); }
    } else { if (!st->connected()) break; delay(1); }
  }
  https.end();
  bodyBuf[len] = 0; outLen = len;
  Serial.printf("  GET %s -> %u bytes in %lu ms (heap %u)\n", url.substring(0, 45).c_str(),
                (unsigned)len, (unsigned long)(millis() - t0), (unsigned)ESP.getFreeHeap());
  return len > 0;
}
static bool awGet(const String& url, JsonDocument& doc, const JsonDocument& filter,
                  const char* referer = nullptr) {
  size_t len;
  if (!httpGetRaw(url, len, BODY_CAP - 1, referer)) return false;
  DeserializationError err = deserializeJson(doc, bodyBuf, len, DeserializationOption::Filter(filter));
  if (err) { lastErr = String("JSON ") + err.c_str(); Serial.printf("  parse: %s\n", err.c_str()); return false; }
  return true;
}
// "2026-09-27T12:57:00.000Z" -> UTC epoch (days-from-civil, Howard Hinnant)
static time_t parseISO(const char* s) {
  int y, mo, d, h, mi, se;
  if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
  y -= mo <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = era * 146097L + (long)doe - 719468L;
  time_t t = (time_t)(days * 86400L + h * 3600L + mi * 60L + se);
  const char* z = s + 19;                       // past "....-..-..T..:..:.."
  while (*z == '.' || (*z >= '0' && *z <= '9')) z++;   // skip fractional seconds
  if (*z == '+' || *z == '-') {                 // NWS stamps carry -06:00 etc, not Z
    int oh = 0, om = 0;
    if (sscanf(z + 1, "%d:%d", &oh, &om) == 2) {
      long off = oh * 3600L + om * 60L;
      t += (*z == '+') ? -off : off;
    }
  }
  return t;
}
static const char* CARD16[] = {"N","NNE","NE","ENE","E","ESE","SE","SSE","S","SSW","SW","WSW","W","WNW","NW","NNW"};
static String cardinal(float deg) {
  if (isnan(deg)) return "";
  int i = ((int)((deg + 11.25f) / 22.5f)) % 16; if (i < 0) i += 16;
  return String(CARD16[i]);
}
// ESP32 newlib's struct tm has no tm_gmtoff, so derive the UTC offset by comparing
// the local and UTC breakdowns of the same instant (handles the day/month wrap).
static long tzOffsetSec(time_t t) {
  struct tm lt, gt;
  localtime_r(&t, &lt); gmtime_r(&t, &gt);
  long d = (lt.tm_hour * 3600L + lt.tm_min * 60L + lt.tm_sec)
         - (gt.tm_hour * 3600L + gt.tm_min * 60L + gt.tm_sec);
  int dd = lt.tm_mday - gt.tm_mday;
  if (dd == 1 || dd < -1) d += 86400;        // local is the next day
  else if (dd == -1 || dd > 1) d -= 86400;   // local is the previous day
  return d;
}
// Sunrise equation (NOAA / Wikipedia), good to ~1-2 min. Returns local minutes from midnight.
static void sunTimes(int y, int mo, int d, double lat, double lon, long tzoff, double& rise, double& set) {
  long a = (14 - mo) / 12, yy = y + 4800 - a, mm = mo + 12 * a - 3;
  long jdn = d + (153 * mm + 2) / 5 + 365 * yy + yy / 4 - yy / 100 + yy / 400 - 32045;
  double n = (double)jdn - 2451545.0 + 0.0008;
  double Js = n - lon / 360.0;
  double M = fmod(357.5291 + 0.98560028 * Js, 360.0), Mr = M * DEG_TO_RAD;
  double C = 1.9148 * sin(Mr) + 0.0200 * sin(2 * Mr) + 0.0003 * sin(3 * Mr);
  double lam = fmod(M + C + 180.0 + 102.9372, 360.0), lr = lam * DEG_TO_RAD;
  double Jtr = 2451545.0 + Js + 0.0053 * sin(Mr) - 0.0069 * sin(2 * lr);
  double sinDec = sin(lr) * sin(23.4397 * DEG_TO_RAD);
  double cosDec = cos(asin(sinDec)), latr = lat * DEG_TO_RAD;
  double cosW = (sin(-0.833 * DEG_TO_RAD) - sin(latr) * sinDec) / (cos(latr) * cosDec);
  if (cosW > 1 || cosW < -1) { rise = set = NAN; return; }   // polar day / night
  double w = acos(cosW) * RAD_TO_DEG;
  auto toLocal = [&](double J) {
    double mins = (J - (double)jdn + 0.5) * 1440.0 + tzoff / 60.0;
    while (mins < 0) mins += 1440; while (mins >= 1440) mins -= 1440;
    return mins;
  };
  rise = toLocal(Jtr - w / 360.0); set = toLocal(Jtr + w / 360.0);
}
static String fmtMin(double m) {
  if (isnan(m)) return "--";
  int t = (int)(m + 0.5), h = (t / 60) % 24, mi = t % 60, hh = h % 12; if (!hh) hh = 12;
  char b[16]; snprintf(b, sizeof b, "%d:%02d %s", hh, mi, h < 12 ? "AM" : "PM"); return String(b);
}
static String agoStr(time_t then, time_t now) {
  if (then <= 0) return "never";
  long s = now - then; if (s < 0) s = 0;
  if (s < 3600) return String(s / 60) + "m ago";
  if (s < 86400) return String(s / 3600) + "h ago";
  return String(s / 86400) + "d ago";
}
static void parseObs(JsonObject o) {
  cur.tempf     = o["tempf"]        | NAN;
  cur.humidity  = o["humidity"]     | NAN;
  cur.feelsLike = o["feelsLike"]    | NAN;
  cur.dewPoint  = o["dewPoint"]     | NAN;
  cur.baromRel  = (o["baromrelin"] | NAN) * INHG2MB;
  cur.baromAbs  = (o["baromabsin"] | NAN) * INHG2MB;
  cur.windMph   = o["windspeedmph"] | NAN;
  cur.windGust  = o["windgustmph"]  | NAN;
  cur.dailyRain = o["dailyrainin"]  | NAN;
  cur.ltgDay    = o["lightning_day"]      | NAN;
  cur.ltgHour   = o["lightning_hour"]     | NAN;
  cur.ltgDist   = o["lightning_distance"] | NAN;
  cur.ltgTime   = (time_t)((o["lightning_time"] | 0.0) / 1000.0);
  cur.windDir   = o["winddir"]            | NAN;
  cur.windDirAvg= o["winddir_avg10m"]     | NAN;
  cur.windAvg   = o["windspdmph_avg10m"]  | NAN;
  cur.maxGust   = o["maxdailygust"]       | NAN;
  cur.uv        = o["uv"]                 | NAN;
  cur.solar     = o["solarradiation"]     | NAN;
  cur.tempIn    = o["tempinf"]            | NAN;
  cur.humIn     = o["humidityin"]         | NAN;
  cur.rainHour  = o["hourlyrainin"]       | NAN;
  cur.rainWeek  = o["weeklyrainin"]       | NAN;
  cur.rainMonth = o["monthlyrainin"]      | NAN;
  cur.rainYear  = o["yearlyrainin"]       | NAN;
  cur.lastRainT = parseISO(o["lastRain"]  | "");
  cur.t         = (time_t)((o["dateutc"] | 0.0) / 1000.0);
  cur.ok        = true;
}
static void computeTrend() {
  if (histN < 2 || !cur.ok || isnan(cur.baromRel)) { trendDelta = NAN; return; }
  time_t target = cur.t - 3 * 3600; int best = -1;
  for (int i = 0; i < histN; i++)
    if (best < 0 || labs((long)(histT[i] - target)) < labs((long)(histT[best] - target))) best = i;
  if (best < 0) return;
  trendDelta = cur.baromRel - histP[best];
  trendSpanMin = (int)((cur.t - histT[best]) / 60);
}
bool fetchCurrent() {
  String url = String("https://rt.ambientweather.net/v1/devices?apiKey=") + AW_API_KEY +
               "&applicationKey=" + AW_APP_KEY;
  JsonDocument filter;
  filter[0]["macAddress"] = true;
  for (auto k : KEEP) filter[0]["lastData"][k] = true;
  JsonDocument doc;
  if (!awGet(url, doc, filter)) return false;
  JsonArray arr = doc.as<JsonArray>();
  if (arr.size() == 0) { lastErr = "no devices"; return false; }
  JsonObject dev = arr[0];
  for (JsonObject d : arr) if (String(d["macAddress"] | "").equalsIgnoreCase(AW_MAC)) { dev = d; break; }
  JsonObject o = dev["lastData"];
  if (o.isNull()) { lastErr = "no lastData"; return false; }
  parseObs(o);
  if (histN > 0 && cur.t > histT[histN - 1] && !isnan(cur.baromRel)) {
    if (histN == HIST_MAX) { memmove(histP, histP + 1, sizeof(float) * (HIST_MAX - 1)); memmove(histT, histT + 1, sizeof(time_t) * (HIST_MAX - 1)); histN--; }
    histP[histN] = cur.baromRel; histT[histN] = cur.t; histN++;
  }
  if (!isnan(cur.tempf) && cur.t > 0) {            // extend today's hi/lo with the new reading
    struct tm lt; localtime_r(&cur.t, &lt);
    if (lt.tm_yday == dayKey) {
      if (isnan(dayHi) || cur.tempf > dayHi) { dayHi = cur.tempf; dayHiT = cur.t; }
      if (isnan(dayLo) || cur.tempf < dayLo) { dayLo = cur.tempf; dayLoT = cur.t; }
    }
  }
  computeTrend(); time(&lastGood); lastErr = ""; return true;
}
bool fetchHistory() {
  String url = String("https://rt.ambientweather.net/v1/devices/") + AW_MAC +
               "?apiKey=" + AW_API_KEY + "&applicationKey=" + AW_APP_KEY +
               "&limit=" + String(HIST_LIMIT);
  JsonDocument filter;                       // history only needs these two - keeps the parsed doc small
  filter[0]["dateutc"] = true; filter[0]["baromrelin"] = true;
  JsonDocument doc;
  if (!awGet(url, doc, filter)) return false;
  JsonArray arr = doc.as<JsonArray>();
  int n = arr.size();
  if (n == 0) { lastErr = "no data"; return false; }
  if (n > HIST_MAX) n = HIST_MAX;
  histN = 0;
  for (int i = n - 1; i >= 0; i--) {
    JsonObject o = arr[i];
    histP[histN] = (o["baromrelin"] | NAN) * INHG2MB;
    histT[histN] = (time_t)((o["dateutc"] | 0.0) / 1000.0);
    histN++;
  }
  if (!cur.ok) { parseObs(arr[0]); time(&lastGood); }
  computeTrend(); lastErr = ""; return true;
}
// ---------------- quotes (Yahoo chart API, keyless) ----------------
// range=1d matters: chartPreviousClose is the close before the RANGE, so a 5d range would
// give a 5-day-old baseline and wrong day changes.
bool fetchQuotes() {
  JsonDocument filter;
  JsonObject m = filter["chart"]["result"][0].to<JsonObject>()["meta"].to<JsonObject>();
  m["regularMarketPrice"] = true; m["chartPreviousClose"] = true; m["regularMarketTime"] = true;
  int ok = 0;
  for (int i = 0; i < NHOLD; i++) {
    String url = String("https://query1.finance.yahoo.com/v8/finance/chart/") + HOLD[i].sym +
                 "?interval=1d&range=1d";
    JsonDocument doc;
    if (!awGet(url, doc, filter)) { Serial.printf("  quote %s FAILED (%s)\n", HOLD[i].sym, lastErr.c_str()); continue; }
    JsonObject meta = doc["chart"]["result"][0]["meta"];
    float px = meta["regularMarketPrice"] | NAN;
    float pc = meta["chartPreviousClose"] | NAN;
    if (isnan(px) || px <= 0) { Serial.printf("  quote %s: no price\n", HOLD[i].sym); continue; }
    HOLD[i].price = px;                                   // keep last good on failure
    if (!isnan(pc) && pc > 0) HOLD[i].prev = pc;
    HOLD[i].at = (time_t)(meta["regularMarketTime"] | 0);
    if (HOLD[i].at > quoteAt) quoteAt = HOLD[i].at;
    ok++;
    delay(120);                                           // be polite to Yahoo
  }
  quotesOk = ok;
  Serial.printf("quotes: %d/%d ok\n", ok, NHOLD);

  int iok = 0;                                   // market context indices
  for (int i = 0; i < NIDX; i++) {
    String url = String("https://query1.finance.yahoo.com/v8/finance/chart/") + IDX[i].path +
                 "?interval=1d&range=1d";
    JsonDocument doc;
    if (!awGet(url, doc, filter)) continue;
    JsonObject meta = doc["chart"]["result"][0]["meta"];
    float px = meta["regularMarketPrice"] | NAN;
    float pc = meta["chartPreviousClose"] | NAN;
    if (isnan(px) || px <= 0) continue;
    IDX[i].price = px;
    if (!isnan(pc) && pc > 0) IDX[i].prev = pc;
    iok++;
    delay(120);
  }
  idxOk = iok;
  Serial.printf("indices: %d/%d ok\n", iok, NIDX);
  return ok > 0;
}

// ---------------- river / quakes / space weather fetches ----------------
bool fetchRiver() {
  char url[256];
  snprintf(url, sizeof url,
           "https://waterservices.usgs.gov/nwis/iv/?format=json&sites=%s"
           "&parameterCd=00065,00060&period=P1D", USGS_SITE);
  JsonDocument filter;
  JsonObject ts = filter["value"]["timeSeries"][0].to<JsonObject>();
  ts["variable"]["variableCode"][0]["value"] = true;
  ts["values"][0]["value"][0]["value"] = true;
  JsonDocument doc;
  if (!awGet(url, doc, filter)) return false;
  rivHistN = 0;
  for (JsonObject t : doc["value"]["timeSeries"].as<JsonArray>()) {
    String code = t["variable"]["variableCode"][0]["value"] | "";
    JsonArray v = t["values"][0]["value"];
    if (v.size() == 0) continue;
    float last = atof(v[v.size() - 1]["value"] | "nan");
    if (code == "00060") { rivFlow = last; continue; }
    if (code != "00065") continue;
    rivStage = last;
    rivFirst = atof(v[0]["value"] | "nan");
    int n = v.size(), stride = n > 48 ? (n + 47) / 48 : 1;   // downsample to <=48 points
    for (int i = 0; i < n && rivHistN < 48; i += stride) {
      float f = atof(v[i]["value"] | "nan");
      if (!isnan(f)) rivHist[rivHistN++] = f;
    }
  }
  if (isnan(rivStage)) { lastErr = "no stage"; return false; }
  time(&rivAt);
  Serial.printf("river: %.2f ft, %.0f ft3/s, %d pts\n", rivStage, rivFlow, rivHistN);
  return true;
}
static float haversineMi(double lat1, double lon1, double lat2, double lon2) {
  const double R = 3958.8;                                   // mean earth radius, miles
  double dLat = (lat2 - lat1) * DEG_TO_RAD, dLon = (lon2 - lon1) * DEG_TO_RAD;
  double a = sin(dLat / 2) * sin(dLat / 2) +
             cos(lat1 * DEG_TO_RAD) * cos(lat2 * DEG_TO_RAD) * sin(dLon / 2) * sin(dLon / 2);
  return (float)(2.0 * R * asin(sqrt(a)));
}
bool fetchQuakes() {
  JsonDocument filter;
  JsonObject ff = filter["features"][0].to<JsonObject>();
  ff["properties"]["mag"] = true; ff["properties"]["place"] = true; ff["properties"]["time"] = true;
  ff["geometry"]["coordinates"] = true;
  // US only (Bradley: "US is more interesting"). The canned summary feeds are global, so use the
  // FDSN query API with a US bounding box - covers the lower 48 plus Alaska and Hawaii. Sorted
  // nearest-first below, so lower-48 events lead and Alaska falls to the bottom.
  // M2.5+/24 h gives ~15 events (~11 KB); the lower 48 alone gives only ~6, too sparse.
  time_t nw; time(&nw);
  struct tm g; gmtime_r(&nw, &g);                       // FDSN wants UTC
  time_t st = nw - 86400; struct tm gs; gmtime_r(&st, &gs);
  char ts[24]; strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", &gs);
  char url[288];
  snprintf(url, sizeof url,
           "https://earthquake.usgs.gov/fdsnws/event/1/query?format=geojson&orderby=time"
           "&minlatitude=17&maxlatitude=72&minlongitude=-180&maxlongitude=-64"
           "&minmagnitude=2.5&starttime=%s", ts);
  JsonDocument doc;
  if (!awGet(url, doc, filter)) return false;
  JsonArray fs = doc["features"];
  qkTotal = fs.size(); qkN = 0; qkBig = 0;
  for (JsonObject f : fs) {
    float m = f["properties"]["mag"] | NAN;
    if (isnan(m)) continue;
    if (m > qkBig) qkBig = m;                                // largest across the WHOLE feed
    JsonArray c = f["geometry"]["coordinates"];
    if (c.size() < 2) continue;
    float d = haversineMi(SITE_LAT, SITE_LON, c[1] | 0.0, c[0] | 0.0);
    // keep the QK_MAX nearest, insertion-sorted by distance
    if (qkN == QK_MAX && d >= QK[qkN - 1].dist) continue;
    int pos = qkN < QK_MAX ? qkN : QK_MAX - 1;
    while (pos > 0 && QK[pos - 1].dist > d) { QK[pos] = QK[pos - 1]; pos--; }
    char raw[96]; strlcpy(raw, f["properties"]["place"] | "", sizeof raw);
    asciiClean(QK[pos].place, sizeof QK[pos].place, raw);
    QK[pos].mag = m; QK[pos].dist = d;
    QK[pos].t = (time_t)((double)(f["properties"]["time"] | 0.0) / 1000.0);
    if (qkN < QK_MAX) qkN++;
  }
  time(&qkAt);
  Serial.printf("quakes(US): %d of %d (M2.5+ 24h), nearest %.0f mi, largest M%.1f\n",
                qkN, qkTotal, qkN ? QK[0].dist : -1.0f, qkBig);
  return qkN > 0;
}
bool fetchSpace() {
  JsonDocument filter;
  filter[0]["Kp"] = true; filter[0]["time_tag"] = true;
  JsonDocument doc;
  if (!awGet("https://services.swpc.noaa.gov/products/noaa-planetary-k-index.json", doc, filter))
    return false;
  JsonArray a = doc.as<JsonArray>();
  if (!a.size()) { lastErr = "no kp"; return false; }
  int n = a.size(), start = n > 24 ? n - 24 : 0;     // last 24 samples = 3 days at 3 h
  kpHistN = 0;
  for (int i = start; i < n; i++) {
    float k = a[i]["Kp"] | NAN;
    if (!isnan(k) && kpHistN < 24) {
      kpTime[kpHistN] = parseISO(a[i]["time_tag"] | "");   // SWPC time_tag is bare UTC
      kpHist[kpHistN++] = k;
    }
  }
  kpNow = kpHistN ? kpHist[kpHistN - 1] : NAN;
  time(&kpAt);
  Serial.printf("space: Kp %.2f (%d samples)\n", kpNow, kpHistN);
  return true;
}

// ---------------- pollen + AQI fetch ----------------
static const char* pollenBand(float v) {
  if (isnan(v)) return "";
  if (v < 2.5f) return "Low";  if (v < 4.9f) return "Low-Med";
  if (v < 7.3f) return "Medium"; if (v < 9.7f) return "Med-High";
  return "High";
}
static const char* aqiBand(int a) {
  if (a <= 50)  return "Good";       if (a <= 100) return "Moderate";
  if (a <= 150) return "Unhealthy-Sens"; if (a <= 200) return "Unhealthy";
  if (a <= 300) return "Very Unhealthy";
  return "Hazardous";
}
bool fetchAir() {
  bool any = false;
  { String url = String("https://www.pollen.com/api/forecast/current/pollen/") + POLLEN_ZIP;
    String ref = String("https://www.pollen.com/forecast/current/pollen/") + POLLEN_ZIP;
    JsonDocument filter;
    JsonObject pf = filter["Location"]["periods"][0].to<JsonObject>();
    pf["Index"] = true; pf["Type"] = true;
    pf["Triggers"][0]["Name"] = true;
    JsonDocument doc;
    if (awGet(url, doc, filter, ref.c_str())) {
      for (JsonObject per : doc["Location"]["periods"].as<JsonArray>()) {
        if (String(per["Type"] | "") != "Today") continue;
        pollenIdx = per["Index"] | NAN;
        pollenTrig[0] = 0;
        int k = 0;
        for (JsonObject tg : per["Triggers"].as<JsonArray>()) {
          const char* nm = tg["Name"] | "";
          if (!nm[0] || k >= 3) continue;                 // three is all that fits on the line
          if (k) strlcat(pollenTrig, ", ", sizeof pollenTrig);
          strlcat(pollenTrig, nm, sizeof pollenTrig);
          k++;
        }
        any = true;
      }
    }
  }
  delay(300);
  { String url = String("https://www.pollen.com/api/forecast/current/asthma/") + POLLEN_ZIP;
    String ref = String("https://www.pollen.com/forecast/current/asthma/") + POLLEN_ZIP;
    JsonDocument filter;
    JsonObject pf = filter["Location"]["periods"][0].to<JsonObject>();
    pf["Index"] = true; pf["Type"] = true;
    JsonDocument doc;
    if (awGet(url, doc, filter, ref.c_str()))
      for (JsonObject per : doc["Location"]["periods"].as<JsonArray>())
        if (String(per["Type"] | "") == "Today") { asthmaIdx = per["Index"] | NAN; any = true; }
  }
  delay(300);
  { String url = String("https://www.pollen.com/api/forecast/extended/pollen/") + POLLEN_ZIP;
    String ref = String("https://www.pollen.com/forecast/extended/pollen/") + POLLEN_ZIP;
    JsonDocument filter;
    JsonObject pf = filter["Location"]["periods"][0].to<JsonObject>();
    pf["Index"] = true; pf["Period"] = true;
    JsonDocument doc;
    if (awGet(url, doc, filter, ref.c_str())) {
      pollen5N = 0;
      for (JsonObject per : doc["Location"]["periods"].as<JsonArray>()) {
        if (pollen5N >= 5) break;
        int yy, mm, dd;
        const char* pd = per["Period"] | "";          // "2026-09-29T15:37:50.027"
        if (sscanf(pd, "%d-%d-%d", &yy, &mm, &dd) != 3) continue;
        pollen5[pollen5N] = per["Index"] | NAN;
        p5mon[pollen5N] = mm; p5day[pollen5N] = dd;
        pollen5N++;
      }
      any = true;
    }
  }
  { char url[224];
    snprintf(url, sizeof url,
             "https://air-quality-api.open-meteo.com/v1/air-quality?latitude=%.4f&longitude=%.4f"
             "&current=us_aqi,pm2_5,pm10,ozone&timezone=America%%2FChicago", SITE_LAT, SITE_LON);
    JsonDocument filter;
    JsonObject cf = filter["current"].to<JsonObject>();
    cf["us_aqi"] = true; cf["pm2_5"] = true; cf["pm10"] = true; cf["ozone"] = true;
    JsonDocument doc;
    if (awGet(url, doc, filter)) {
      aqiVal = doc["current"]["us_aqi"] | -1;
      pm25   = doc["current"]["pm2_5"]  | NAN;
      pm10   = doc["current"]["pm10"]   | NAN;
      ozone  = doc["current"]["ozone"]  | NAN;
      any = true;
    }
  }
  if (any) time(&airAt);
  Serial.printf("air: pollen %.1f (%s) [%s]  asthma %.1f  AQI %d  PM2.5 %.1f  %d-day fc\n",
                pollenIdx, pollenBand(pollenIdx), pollenTrig, asthmaIdx, aqiVal, pm25, pollen5N);
  return any;
}

// ---------------- WSJ headlines fetch ----------------
// The GFX fonts are ASCII-only, so curly quotes / dashes / ellipses must be transliterated or
// they render as blanks. Also decodes the handful of XML entities Google News emits.
static void asciiClean(char* d, size_t cap, const char* s) {
  size_t o = 0;
  for (size_t i = 0; s[i] && o + 1 < cap; ) {
    unsigned char c = (unsigned char)s[i];
    if (c == '&') {
      if (!strncmp(s + i, "&amp;",  5)) { d[o++] = '&';  i += 5; continue; }
      if (!strncmp(s + i, "&quot;", 6)) { d[o++] = '"';  i += 6; continue; }
      if (!strncmp(s + i, "&#39;",  5)) { d[o++] = '\''; i += 5; continue; }
      if (!strncmp(s + i, "&apos;", 6)) { d[o++] = '\''; i += 6; continue; }
      if (!strncmp(s + i, "&lt;",   4)) { d[o++] = '<';  i += 4; continue; }
      if (!strncmp(s + i, "&gt;",   4)) { d[o++] = '>';  i += 4; continue; }
      d[o++] = '&'; i++; continue;
    }
    if (c < 0x80) { d[o++] = (char)c; i++; continue; }
    if (c == 0xE2 && (unsigned char)s[i + 1] == 0x80) {          // U+2018..U+2026
      unsigned char k = (unsigned char)s[i + 2];
      const char* r = (k == 0x98 || k == 0x99) ? "'" :
                      (k == 0x9C || k == 0x9D) ? "\"" :
                      (k == 0x93 || k == 0x94) ? "-" :
                      (k == 0xA6) ? "..." : "";
      while (*r && o + 1 < cap) d[o++] = *r++;
      i += 3; continue;
    }
    i++; while ((s[i] & 0xC0) == 0x80) i++;                      // skip other multibyte
  }
  d[o] = 0;
}
static time_t parseRFC822(const char* s) {                       // "Tue, 29 Sep 2026 13:50:00 GMT"
  static const char* MON = "JanFebMarAprMayJunJulAugSepOctNovDec";
  int d, y, h, mi, se; char mon[8];
  const char* c = strchr(s, ','); if (c) s = c + 1;
  while (*s == ' ') s++;
  if (sscanf(s, "%d %3s %d %d:%d:%d", &d, mon, &y, &h, &mi, &se) != 6) return 0;
  const char* q = strstr(MON, mon); if (!q) return 0;
  int mo = (int)((q - MON) / 3) + 1;
  int yy = y - (mo <= 2);
  long era = (yy >= 0 ? yy : yy - 399) / 400;
  unsigned yoe = (unsigned)(yy - era * 400);
  unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = era * 146097L + (long)doe - 719468L;
  return (time_t)(days * 86400L + h * 3600L + mi * 60L + se);    // feed stamps are GMT
}
bool fetchNews() {
  size_t len;
  if (!httpGetRaw(NEWS_URL, len, 16000)) return false;
  int n = 0;
  const char* p = bodyBuf;
  while (n < NEWS_MAX && (p = strstr(p, "<item>")) != nullptr) {
    const char* end = strstr(p, "</item>");
    const char* t   = strstr(p, "<title>");
    const char* te  = t ? strstr(t, "</title>") : nullptr;
    if (!t || !te || (end && t > end)) break;                    // truncated mid-item
    char raw[200];
    size_t l = (size_t)(te - (t + 7)); if (l >= sizeof raw) l = sizeof raw - 1;
    memcpy(raw, t + 7, l); raw[l] = 0;
    char clean[124];
    asciiClean(clean, sizeof clean, raw);
    // Google News always appends " - <source>", but the label varies (" - wsj.com", " - WSJ",
    // " - The Wall Street Journal"), so truncate at the LAST separator rather than matching names.
    for (char* q = clean + strlen(clean) - 3; q > clean; q--)
      if (q[0] == ' ' && q[1] == '-' && q[2] == ' ') { *q = 0; break; }
    if (!strstr(clean, "Print Edition") && clean[0]) {           // skip the daily index entry
      strlcpy(NEWS[n].title, clean, sizeof NEWS[n].title);
      const char* d = strstr(p, "<pubDate>");
      NEWS[n].t = (d && (!end || d < end)) ? parseRFC822(d + 9) : 0;
      n++;
    }
    if (!end) break;
    p = end + 7;
  }
  if (n == 0) { lastErr = "no items"; return false; }
  newsN = n; time(&newsAt);
  Serial.printf("news: %d headlines, top='%s'\n", newsN, NEWS[0].title);
  return true;
}

// ---------------- NWS alerts fetch ----------------
bool fetchAlerts() {
  char url[128];
  snprintf(url, sizeof url, "https://api.weather.gov/alerts/active?point=%.4f,%.4f", SITE_LAT, SITE_LON);
  JsonDocument filter;
  JsonObject pf = filter["features"][0]["properties"].to<JsonObject>();
  pf["event"] = true; pf["ends"] = true; pf["expires"] = true; pf["areaDesc"] = true;
  JsonDocument doc;
  if (!awGet(url, doc, filter)) return false;

  char ev[48] = "", ar[72] = ""; time_t en = 0; int rank = 0;
  for (JsonObject a : doc["features"].as<JsonArray>()) {
    const char* e = a["properties"]["event"] | "";
    if (!strstr(e, "Warning")) continue;                 // warnings only
    int r = strstr(e, "Tornado") ? 2 : (strstr(e, "Thunderstorm") ? 1 : 0);
    if (r <= rank) continue;                             // keep the most severe
    rank = r;
    strlcpy(ev, e, sizeof ev);
    strlcpy(ar, a["properties"]["areaDesc"] | "", sizeof ar);
    const char* t = a["properties"]["ends"] | "";
    if (!t[0]) t = a["properties"]["expires"] | "";
    en = parseISO(t);
  }
  bool was = alertActive; char prev[48]; strlcpy(prev, alertEvent, sizeof prev);
  alertRank = rank; alertActive = rank > 0;
  strlcpy(alertEvent, ev, sizeof alertEvent);
  strlcpy(alertArea,  ar, sizeof alertArea);
  alertEnds = en;
  if (alertActive != was || strcmp(prev, alertEvent) != 0) {
    if (alertActive) { curScreen = SCR_WEATHER; Serial.printf("*** ALERT: %s (%s) until %s\n", alertEvent, alertArea, fmtTime(alertEnds).c_str()); }
    else Serial.println("alert cleared");
    needRender = true;                                   // repaint immediately, don't wait for rotation
  }
  return true;
}
// An alert whose end time has passed is over even if the next poll hasn't run yet.
static bool alertLive() {
  if (!alertActive) return false;
  if (alertEnds > 0) { time_t n; time(&n); if (n > alertEnds) return false; }
  return true;
}

// ---------------- NWS forecast fetch ----------------
bool fetchForecast() {
  JsonDocument filter;
  JsonObject pf = filter["properties"]["periods"][0].to<JsonObject>();
  pf["name"] = true; pf["temperature"] = true; pf["isDaytime"] = true;
  pf["shortForecast"] = true; pf["detailedForecast"] = true;
  pf["windSpeed"] = true; pf["windDirection"] = true;
  pf["probabilityOfPrecipitation"]["value"] = true;
  JsonDocument doc;
  if (!awGet(NWS_URL, doc, filter)) return false;
  JsonArray ps = doc["properties"]["periods"];
  if (!ps.size()) { lastErr = "no periods"; return false; }
  fcN = 0;
  for (JsonObject o : ps) {
    if (fcN >= 5) break;
    Period& q = fc[fcN];
    strlcpy(q.name,   o["name"]          | "", sizeof q.name);
    strlcpy(q.shortF, o["shortForecast"] | "", sizeof q.shortF);
    const char* wsp = o["windSpeed"]     | "";
    const char* wdr = o["windDirection"] | "";
    snprintf(q.wind, sizeof q.wind, "%s %s", wsp, wdr);
    q.temp = o["temperature"] | 0;
    q.day  = o["isDaytime"]   | true;
    q.pop  = o["probabilityOfPrecipitation"]["value"] | 0;
    if (fcN == 0) strlcpy(fcDetail, o["detailedForecast"] | "", sizeof fcDetail);
    fcN++;
  }
  time(&fcAt);
  Serial.printf("forecast: %d periods, '%s' %dF %s\n", fcN, fc[0].name, fc[0].temp, fc[0].shortF);
  return true;
}

// ---------------- ComEd fetch ----------------
bool fetchComEd() {
  bool any = false;
  { JsonDocument filter; filter[0]["price"] = true; filter[0]["millisUTC"] = true;
    JsonDocument doc;
    if (awGet("https://hourlypricing.comed.com/api?type=currenthouraverage", doc, filter)) {
      JsonArray arr = doc.as<JsonArray>();
      if (arr.size()) {
        comedCur = atof(arr[0]["price"]  | "0");
        comedAt  = (time_t)(atof(arr[0]["millisUTC"] | "0") / 1000.0);
        any = true;
      }
    }
  }
  time_t now; time(&now); struct tm lt; localtime_r(&now, &lt);
  char url[96];
  snprintf(url, sizeof url, "https://hourlypricing.comed.com/api?type=day&date=%04d%02d%02d",
           lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
  size_t len;
  if (httpGetRaw(url, len)) {
    for (int i = 0; i < 24; i++) comedHour[i] = NAN;
    comedN = 0;
    const char* q = bodyBuf;
    while ((q = strstr(q, "Date.UTC(")) != nullptr) {
      q += 9;
      int y, mo, d, h, mi, se;
      if (sscanf(q, "%d,%d,%d,%d,%d,%d", &y, &mo, &d, &h, &mi, &se) == 6) {
        const char* r = strchr(q, ')');
        if (r) { r++; while (*r == ',' || *r == ' ') r++;
                 if (h >= 0 && h < 24) { comedHour[h] = atof(r); comedN++; } }
      }
    }
    Serial.printf("ComEd: cur %.1fc, %d hourly averages today\n", comedCur, comedN);
    any = true;
  }
  if (!any) Serial.printf("ComEd failed: %s\n", lastErr.c_str());
  return any;
}

// Page back through today's history accumulating hi/lo. Stops once it crosses local midnight.
void backfillDayHiLo() {
  time_t now; time(&now);
  if (now < 1000000000) return;                       // wait for NTP
  struct tm lt; localtime_r(&now, &lt);
  int key = lt.tm_yday;
  struct tm mid = lt; mid.tm_hour = 0; mid.tm_min = 0; mid.tm_sec = 0; mid.tm_isdst = -1;
  time_t midnight = mktime(&mid);
  dayHi = dayLo = NAN; dayHiT = dayLoT = 0;
  // endDate is epoch MILLISECONDS (13 digits, > 32 bits). ESP32's newlib-nano printf has no
  // %lld, so format the SECONDS with %lu and tack on the three zeros.
  unsigned long endSec = (unsigned long)now;
  JsonDocument filter;
  filter[0]["dateutc"] = true; filter[0]["tempf"] = true;
  int c = 0;
  delay(1400);                       // AWN allows ~1 req/s and we just did history + current
  for (; c < 7; c++) {
    char url[384];
    snprintf(url, sizeof url,
             "https://rt.ambientweather.net/v1/devices/%s?apiKey=%s&applicationKey=%s&limit=48&endDate=%lu000",
             AW_MAC, AW_API_KEY, AW_APP_KEY, endSec);
    JsonDocument doc;
    bool got = false;
    for (int a = 0; a < 3 && !got; a++) {          // 429 is common here - back off and retry
      if (a) delay(2500);
      got = awGet(url, doc, filter);
    }
    if (!got) break;
    JsonArray arr = doc.as<JsonArray>();
    if (!arr.size()) break;
    unsigned long oldestSec = endSec;
    for (JsonObject o : arr) {
      double ms = o["dateutc"] | 0.0;
      float t = o["tempf"] | NAN;
      if (ms <= 0) continue;
      unsigned long sec = (unsigned long)(ms / 1000.0);
      if (sec < oldestSec) oldestSec = sec;
      if ((time_t)sec < midnight || isnan(t)) continue;
      if (isnan(dayHi) || t > dayHi) { dayHi = t; dayHiT = (time_t)sec; }
      if (isnan(dayLo) || t < dayLo) { dayLo = t; dayLoT = (time_t)sec; }
    }
    if (oldestSec <= (unsigned long)midnight) { c++; break; }
    endSec = oldestSec - 1;
    delay(1200);                                      // AWN allows ~1 request/second
  }
  dayKey = key;
  Serial.printf("day hi/lo: %.1f at %s / %.1f at %s (%d chunks)\n",
                dayHi, fmtTime(dayHiT).c_str(), dayLo, fmtTime(dayLoT).c_str(), c);
}

static bool tryN(bool (*fn)(), const char* what, int tries = 3) {
  for (int a = 0; a < tries; a++) { fetchTotal++; if (fn()) { fetchOk++; return true; } delay(1200); }
  Serial.printf("%s failed: %s\n", what, lastErr.c_str());
  return false;
}

// ---------------- e-paper drawing ----------------
// Text helpers: GFX fonts draw from a baseline; use bounds for right/center alignment.
static int16_t textW(const String& s) { int16_t x, y; uint16_t w, h; display.getTextBounds(s, 0, 0, &x, &y, &w, &h); return w; }
static void txt(int x, int y, const String& s, const GFXfont* f, uint16_t col = GxEPD_BLACK, uint8_t size = 1) {
  display.setFont(f); display.setTextSize(size); display.setTextColor(col); display.setCursor(x, y); display.print(s);
}
static void txtR(int xr, int y, const String& s, const GFXfont* f, uint16_t col = GxEPD_BLACK, uint8_t size = 1) {
  display.setFont(f); display.setTextSize(size); txt(xr - textW(s), y, s, f, col, size);
}
static void txtC(int xc, int y, const String& s, const GFXfont* f, uint16_t col = GxEPD_BLACK, uint8_t size = 1) {
  display.setFont(f); display.setTextSize(size); txt(xc - textW(s) / 2, y, s, f, col, size);
}
static void label(int x, int y, const char* s) { txt(x, y, s, &FreeSans9pt7b); }
static void degSign(int x, int y, int r, uint16_t col) { display.drawCircle(x, y, r, col); display.drawCircle(x, y, r - 1, col); }

static void drawArrow(int cx, int cy, float d, uint16_t col) {
  if (isnan(d) || fabs(d) < TREND_MB) { display.fillRect(cx - 18, cy - 3, 36, 6, col); return; }
  if (d > 0) display.fillTriangle(cx - 20, cy + 18, cx + 20, cy + 18, cx, cy - 20, col);
  else       display.fillTriangle(cx - 20, cy - 18, cx + 20, cy - 18, cx, cy + 20, col);
}
static void drawBolt(int x, int y, uint16_t col) {   // ~16x28 lightning bolt
  display.fillTriangle(x + 9, y, x + 1, y + 16, x + 9, y + 15, col);
  display.fillTriangle(x + 9, y + 12, x + 7, y + 28, x + 15, y + 11, col);
}
static void drawSparkline(int x, int y, int w, int h) {
  display.drawRect(x, y, w, h, GxEPD_BLACK);
  if (histN < 2) { txtC(x + w / 2, y + h / 2 + 6, "no history yet", &FreeSans12pt7b); return; }
  float mn = 1e9, mx = -1e9;
  for (int i = 0; i < histN; i++) { if (isnan(histP[i])) continue; mn = min(mn, histP[i]); mx = max(mx, histP[i]); }
  if (mx - mn < 1.0f) { float m = (mx + mn) / 2; mn = m - 0.5f; mx = m + 0.5f; }
  // light horizontal grid (dotted) at quarter heights
  for (int g = 1; g < 4; g++) { int gy = y + g * h / 4; for (int gx = x + 4; gx < x + w - 4; gx += 6) display.drawPixel(gx, gy, GxEPD_BLACK); }
  int px = -1, py = -1;
  for (int i = 0; i < histN; i++) {
    if (isnan(histP[i])) continue;
    int gx = x + 6 + (int)((float)i / (histN - 1) * (w - 12));
    int gy = y + h - 6 - (int)((histP[i] - mn) / (mx - mn) * (h - 12));
    if (px >= 0) { display.drawLine(px, py, gx, gy, GxEPD_BLACK); display.drawLine(px, py + 1, gx, gy + 1, GxEPD_BLACK); display.drawLine(px, py - 1, gx, gy - 1, GxEPD_BLACK); }
    px = gx; py = gy;
  }
  display.fillCircle(px, py, 5, GxEPD_RED);                      // latest point
  txtR(x + w - 6, y + 16, f1(mx), &FreeSans9pt7b);
  txtR(x + w - 6, y + h - 6, f1(mn), &FreeSans9pt7b);
}
// A trailing '~' in val means "draw a degree sign here" (the GFX fonts are ASCII-only).
static void hiLoBlock(int x, int y, const char* lab, float v, time_t t) {
  txt(x, y, lab, &FreeSans9pt7b);
  String sv = isnan(v) ? "--" : f0(v);
  txt(x, y + 38, sv, &FreeSansBold18pt7b);
  display.setFont(&FreeSansBold18pt7b); display.setTextSize(1);
  int w = textW(sv);
  if (!isnan(v)) degSign(x + w + 7, y + 22, 4, GxEPD_BLACK);
  if (t) txt(x + w + 18, y + 38, fmtTime(t), &FreeSans9pt7b);
}
static void statBlockSm(int x, int y, const char* lab, String val, uint16_t col = GxEPD_BLACK) {
  label(x, y, lab);
  txt(x, y + 30, val, &FreeSansBold12pt7b, col);
}
static void statBlock(int x, int y, const char* lab, String val, uint16_t col = GxEPD_BLACK) {
  label(x, y, lab);
  bool deg = val.endsWith("~");
  if (deg) val.remove(val.length() - 1);
  txt(x, y + 34, val, &FreeSansBold18pt7b, col);
  if (deg) { display.setFont(&FreeSansBold18pt7b); display.setTextSize(1); degSign(x + textW(val) + 8, y + 16, 4, col); }
}


// Message markup: *words between asterisks* are drawn in RED; everything else black.
static String stripMarks(const String& s) { String o = s; o.replace("*", ""); return o; }

// Word-wrap s (spaces and explicit '\n') into out[] for the given font/size within maxW px.
// Widths are measured with the '*' markers removed so markup never changes the wrapping.
static int wrapText(const String& s, const GFXfont* f, uint8_t size, int maxW, String out[], int maxLines) {
  display.setFont(f); display.setTextSize(size);
  int n = 0; String line;
  int i = 0, len = s.length();
  while (i <= len && n < maxLines) {
    int j = i; while (j < len && s[j] != ' ' && s[j] != '\n') j++;
    String word = s.substring(i, j);
    String cand = line.length() ? line + " " + word : word;
    if (textW(stripMarks(cand)) > maxW && line.length()) { out[n++] = line; line = word; }
    else line = cand;
    if (j >= len) { if (line.length() && n < maxLines) out[n++] = line; break; }
    if (s[j] == '\n') { if (n < maxLines) out[n++] = line; line = ""; }
    i = j + 1;
  }
  return n;
}
// Draw one wrapped line, toggling black/red at each '*'. `red` carries the state across lines.
static void drawMarkupLine(int x, int y, const String& line, const GFXfont* f, uint8_t size, bool& red) {
  display.setFont(f); display.setTextSize(size);
  int start = 0;
  for (int i = 0; i <= (int)line.length(); i++) {
    if (i == (int)line.length() || line[i] == '*') {
      String seg = line.substring(start, i);
      if (seg.length()) {
        display.setTextColor(red ? GxEPD_RED : GxEPD_BLACK);
        display.setCursor(x, y); display.print(seg);
        x = display.getCursorX();          // cursor advance keeps the trailing space; ink width (textW) drops it
      }
      if (i < (int)line.length()) red = !red;
      start = i + 1;
    }
  }
}
static String msgExpiryText() {
  if (!msgTtlMs) return "until cleared";
  long left = (long)(msgTtlMs - (millis() - msgSetMs)) / 60000L;
  if (left < 1) return "expiring";
  return left < 60 ? String(left) + " min left" : String(left / 60) + " h " + String(left % 60) + " min left";
}
static void drawFullMessage(time_t now) {
  display.drawRect(24, 92, SCR_W - 48, SCR_H - 132, GxEPD_RED);
  display.drawRect(26, 94, SCR_W - 52, SCR_H - 136, GxEPD_RED);
  String lines[10]; int n; uint8_t size = 2; int lh = 78;
  n = wrapText(msgText, &FreeSansBold24pt7b, 2, SCR_W - 120, lines, 10);
  if (n > 4) { size = 1; lh = 44; n = wrapText(msgText, &FreeSansBold24pt7b, 1, SCR_W - 120, lines, 8); }
  int y0 = 92 + ((SCR_H - 132) - n * lh) / 2 + lh - (size == 2 ? 14 : 8);
  bool red = false;
  for (int i = 0; i < n; i++) {
    display.setFont(&FreeSansBold24pt7b); display.setTextSize(size);
    int x = SCR_W / 2 - textW(stripMarks(lines[i])) / 2;
    drawMarkupLine(x, y0 + i * lh, lines[i], &FreeSansBold24pt7b, size, red);
  }
  display.setTextSize(1);
  txt(24, SCR_H - 14, "Message  -  " + msgExpiryText() + "     " + fmtTime(now), &FreeSans9pt7b);
  txtR(SCR_W - 24, SCR_H - 14, f0(cur.tempf) + " F   " + f1(cur.baromRel) + " mb", &FreeSans9pt7b);
}
static void drawBannerMessage() {
  const int bx = 24, by = 346, bw = SCR_W - 48, bh = 146;
  display.drawRect(bx, by, bw, bh, GxEPD_RED);
  display.drawRect(bx + 1, by + 1, bw - 2, bh - 2, GxEPD_RED);
  display.fillRect(bx, by, 10, bh, GxEPD_RED);
  String lines[4]; int n = wrapText(msgText, &FreeSansBold18pt7b, 1, bw - 60, lines, 3);
  int y0 = by + 40 + ((bh - 30) - n * 36) / 2;
  bool red = false;
  for (int i = 0; i < n; i++) drawMarkupLine(bx + 28, y0 + i * 36, lines[i], &FreeSansBold18pt7b, 1, red);
  txtR(bx + bw - 10, by + bh - 8, msgExpiryText(), &FreeSans9pt7b, GxEPD_RED);
}

// ---------------- portfolio drawing ----------------
static String f2(float v){ char b[16]; if(isnan(v)) return "--"; snprintf(b,sizeof b,"%.2f",v); return String(b); }
static String grpAbs(double v){                     // 1234567 -> "1,234,567"
  char b[24]; snprintf(b,sizeof b,"%ld",(long)llround(fabs(v)));
  String d(b), o; int c=0;
  for (int i=d.length()-1;i>=0;i--){ o = String(d[i]) + o; if(++c%3==0 && i>0) o = "," + o; }
  return o;
}
static String usd (double v){ return String(v<0?"-$":"$") + grpAbs(v); }
static String usdS(double v){ return String(v<0?"-$":"+$") + grpAbs(v); }
static String pctS(double v){ char b[16]; if (fabs(v) < 0.005) return String("0.00%");   // -0.09 on $80k is flat, not a loss
  snprintf(b,sizeof b,"%+.2f%%",v); return String(b); }
// US equity session: 9:30-16:00 ET = 8:30-15:00 CT (both observe DST, so the offset is constant).
// No holiday table needed: on a holiday the ETF quotes never refresh, so the staleness check
// below catches it and correctly reports CLOSED.
static bool marketOpen() {
  time_t now; time(&now);
  struct tm lt; localtime_r(&now, &lt);
  if (lt.tm_wday == 0 || lt.tm_wday == 6) return false;
  int mins = lt.tm_hour * 60 + lt.tm_min;
  if (mins < 8 * 60 + 30 || mins >= 15 * 60) return false;
  if (quoteAt > 0 && (now - quoteAt) > 20 * 60) return false;   // stale = holiday / feed down
  return true;
}
static uint16_t signCol(double v){ return v < 0 ? GxEPD_RED : GxEPD_BLACK; }

void drawPortfolio() {
  time_t now; time(&now);
  display.fillScreen(GxEPD_WHITE);
  display.setTextWrap(false);
  txt(24, 52, "Portfolio", &FreeSansBold24pt7b);
  display.setFont(&FreeSansBold24pt7b); display.setTextSize(1);
  int titleW = textW("Portfolio");
  bool mopen = marketOpen();
  txt(24 + titleW + 24, 52, mopen ? "MARKET OPEN" : "MARKET CLOSED",
      &FreeSansBold12pt7b, mopen ? GxEPD_BLACK : GxEPD_RED);
  // Portfolio runs on EASTERN (market time, and the zone Bradley's clients work in) while the
  // rest of the panel stays Central. Both zones observe DST on the same dates, so ET is exactly
  // CT + 1 h year round - no tzset() juggling inside the paged render loop.
  txtR(SCR_W - 24, 52, fmtTime(now + 3600, "%a %b %e   %l:%M %p") + " ET", &FreeSans12pt7b);
  display.fillRect(24, 68, SCR_W - 48, 3, GxEPD_BLACK);

  double mval = 0, day = 0, cost = 0;
  for (int i = 0; i < NHOLD; i++) {
    if (HOLD[i].price <= 0) continue;
    mval += (double)HOLD[i].qty * HOLD[i].price;
    cost += HOLD[i].cost;
    if (HOLD[i].prev > 0) day += (double)HOLD[i].qty * (HOLD[i].price - HOLD[i].prev);
  }
  double val = mval + CASH_USD, gain = mval - cost;   // treasury is a separate account - excluded
  double dayPct  = (val - day) > 0 ? day / (val - day) * 100.0 : 0;
  double gainPct = cost > 0 ? gain / cost * 100.0 : 0;

  label(24, 88, "TOTAL VALUE");
  txt(20, 162, usd(val), &FreeSansBold24pt7b, GxEPD_BLACK, 2);   // 24pt x2 is ~68px tall: baseline must clear the label at 96
  label(560, 88, "TODAY");
  txt(560, 120, usdS(day) + "   " + pctS(dayPct), &FreeSansBold18pt7b, signCol(day));
  label(560, 146, "TOTAL GAIN / LOSS");
  txt(560, 178, usdS(gain) + "   " + pctS(gainPct), &FreeSansBold18pt7b, signCol(gain));
  display.fillRect(24, 190, SCR_W - 48, 2, GxEPD_BLACK);

  label(24, 210, "SYMBOL");
  txtR(300, 210, "LAST",   &FreeSans9pt7b);
  txtR(430, 210, "DAY",    &FreeSans9pt7b);
  txtR(545, 210, "DAY %",  &FreeSans9pt7b);
  txtR(700, 210, "VALUE",  &FreeSans9pt7b);
  txtR(856, 210, "TOTAL %",&FreeSans9pt7b);

  int y = 234;
  for (int i = 0; i < NHOLD; i++, y += 28) {
    Holding& h = HOLD[i];
    txt(24, y, h.sym, &FreeSansBold12pt7b);
    if (h.price <= 0) { txtR(700, y, "no quote", &FreeSans12pt7b, GxEPD_RED); continue; }
    double v = (double)h.qty * h.price;
    double d = h.prev > 0 ? (double)h.qty * (h.price - h.prev) : 0;
    double dp = h.prev > 0 ? (h.price / h.prev - 1) * 100.0 : 0;
    double g = v - h.cost, gp = h.cost > 0 ? g / h.cost * 100.0 : 0;
    txtR(300, y, f2(h.price), &FreeSans12pt7b);
    txtR(430, y, usdS(d),  &FreeSans12pt7b, signCol(d));
    txtR(545, y, pctS(dp), &FreeSans12pt7b, signCol(d));
    txtR(700, y, usd(v),   &FreeSansBold12pt7b);
    txtR(856, y, pctS(gp), &FreeSans12pt7b, gp < -0.005 ? GxEPD_RED : GxEPD_BLACK);
  }
  display.drawFastHLine(24, y - 18, SCR_W - 48, GxEPD_BLACK);
  txt(24, y + 2, "CASH", &FreeSansBold12pt7b);
  txtR(700, y + 2, usd(CASH_USD), &FreeSansBold12pt7b);
  txt(24, y + 28, "TREASURY", &FreeSansBold12pt7b);
  txt(160, y + 28, "separate account - not included in the total above", &FreeSans9pt7b);
  txtR(700, y + 28, usd(TREASURY_USD), &FreeSansBold12pt7b);

  // ---- market context: is my day market-wide or my own? ----
  // Fixed 166 px columns overlapped ("NASD 26,853 +0.1%" ran into VIX), so measure each entry
  // and share the leftover space out as equal gaps - safe for any value length.
  display.drawFastHLine(24, 468, SCR_W - 48, GxEPD_BLACK);
  {
    String head[NIDX], chg[NIDX]; int wEnt[NIDX], total = 0;
    display.setFont(&FreeSans9pt7b); display.setTextSize(1);
    for (int i = 0; i < NIDX; i++) {
      if (IDX[i].price <= 0) { head[i] = String(IDX[i].label) + " --"; chg[i] = ""; }
      else {
        String v = IDX[i].dec == 0 ? grpAbs(IDX[i].price) : String(IDX[i].price, IDX[i].dec);
        if (IDX[i].pctUnit) v += "%";
        head[i] = String(IDX[i].label) + " " + v;
        if (IDX[i].prev > 0) {
          char b2[12]; snprintf(b2, sizeof b2, "%+.1f%%", (IDX[i].price / IDX[i].prev - 1) * 100.0);
          chg[i] = b2;
        }
      }
      wEnt[i] = textW(head[i]) + (chg[i].length() ? 6 + textW(chg[i]) : 0);
      total += wEnt[i];
    }
    int gap = NIDX > 1 ? ((SCR_W - 48) - total) / (NIDX - 1) : 0;
    if (gap < 8) gap = 8;
    int x = 24;
    for (int i = 0; i < NIDX; i++) {
      txt(x, 494, head[i], &FreeSans9pt7b);
      if (chg[i].length()) {
        display.setFont(&FreeSans9pt7b); display.setTextSize(1);
        txt(x + textW(head[i]) + 6, 494, chg[i], &FreeSans9pt7b,
            chg[i][0] == '-' ? GxEPD_RED : GxEPD_BLACK);
      }
      x += wEnt[i] + gap;
    }
  }

  String foot = String("Quotes ") + String(quotesOk) + "/" + String(NHOLD) +
                (quoteAt > 0 ? String("   as of ") + fmtTime(quoteAt + 3600) : String("")) +
                "   Updated " + fmtTime(lastGood + 3600) + " ET   " + WiFi.localIP().toString();
  txt(24, SCR_H - 14, foot, &FreeSans9pt7b);
  txtR(SCR_W - 24, SCR_H - 14, "delayed quotes - not advice", &FreeSans9pt7b);
}

// ---------------- WSJ headlines drawing ----------------
void drawNews() {
  time_t now; time(&now);
  display.fillScreen(GxEPD_WHITE);
  display.setTextWrap(false);
  txt(24, 52, "Wall Street Journal", &FreeSansBold24pt7b);
  txtR(SCR_W - 24, 52, fmtTime(now + 3600, "%a %b %e   %l:%M %p") + " ET", &FreeSans12pt7b);
  display.fillRect(24, 68, SCR_W - 48, 3, GxEPD_BLACK);

  if (newsN == 0) {
    txtC(SCR_W / 2, 260, "Headlines unavailable", &FreeSansBold18pt7b, GxEPD_RED);
    txt(24, SCR_H - 14, "news.google.com", &FreeSans9pt7b);
    return;
  }
  int y = 108;
  for (int i = 0; i < newsN && y < 484; i++) {
    String ln[2];
    int n = wrapText(NEWS[i].title, &FreeSans12pt7b, 1, SCR_W - 116 - 24, ln, 2);
    if (NEWS[i].t) txt(24, y, agoStr(NEWS[i].t, now), &FreeSans9pt7b);
    for (int k = 0; k < n; k++) txt(116, y + k * 25, ln[k], &FreeSans12pt7b);
    y += n * 25 + 14;
  }
  txt(24, SCR_H - 14, "WSJ headlines via news.google.com    updated " + fmtTime(newsAt + 3600) + " ET",
      &FreeSans9pt7b);
  txtR(SCR_W - 24, SCR_H - 14, String(newsN) + " stories", &FreeSans9pt7b);
}

// ---------------- river / quakes / space drawing ----------------
static const char* kpBand(float k) {
  if (isnan(k)) return "";
  if (k < 3) return "Quiet";      if (k < 4) return "Unsettled";
  if (k < 5) return "Active";     if (k < 6) return "Minor storm G1";
  if (k < 7) return "Moderate G2";if (k < 8) return "Strong G3";
  if (k < 9) return "Severe G4";
  return "Extreme G5";
}
void drawRiver() {
  time_t now; time(&now);
  display.fillScreen(GxEPD_WHITE);
  display.setTextWrap(false);
  txt(24, 52, "Des Plaines River", &FreeSansBold24pt7b);
  txtR(SCR_W - 24, 52, fmtTime(now, "%a %b %e   %l:%M %p"), &FreeSans12pt7b);
  display.fillRect(24, 68, SCR_W - 48, 3, GxEPD_BLACK);

  bool hi = !isnan(FLOOD_ACTION) && !isnan(rivStage) && rivStage >= FLOOD_ACTION;
  label(24, 96, "GAGE HEIGHT");
  String sv = isnan(rivStage) ? "--" : String(rivStage, 2);
  txt(20, 168, sv, &FreeSansBold24pt7b, hi ? GxEPD_RED : GxEPD_BLACK, 2);
  display.setFont(&FreeSansBold24pt7b); display.setTextSize(2);
  int sw = textW(sv); display.setTextSize(1);
  txt(20 + sw + 16, 168, "ft", &FreeSansBold18pt7b, hi ? GxEPD_RED : GxEPD_BLACK);

  label(470, 96, "STREAMFLOW");
  txt(466, 168, isnan(rivFlow) ? "--" : grpAbs(rivFlow), &FreeSansBold24pt7b, GxEPD_BLACK, 2);
  display.setFont(&FreeSansBold24pt7b); display.setTextSize(2);
  int fw = textW(isnan(rivFlow) ? "--" : grpAbs(rivFlow)); display.setTextSize(1);
  txt(466 + fw + 16, 168, "ft3/s", &FreeSansBold18pt7b);

  if (!isnan(rivStage) && !isnan(rivFirst)) {
    float d = rivStage - rivFirst;
    String t = (d > 0 ? "+" : "") + String(d, 2) + " ft in 24 h";
    txt(24, 214, (d > 0.02f ? "RISING    " : d < -0.02f ? "FALLING    " : "STEADY    ") + t,
        &FreeSans12pt7b, d > 0.02f ? GxEPD_RED : GxEPD_BLACK);
  }
  display.fillRect(24, 232, SCR_W - 48, 2, GxEPD_BLACK);

  label(24, 258, "GAGE HEIGHT, LAST 24 HOURS  (ft)");
  const int X0 = 78, X1 = SCR_W - 24, Y0 = 274, Y1 = 462;
  if (rivHistN >= 2) {
    float mn = 1e9, mx = -1e9;
    for (int i = 0; i < rivHistN; i++) { mn = min(mn, rivHist[i]); mx = max(mx, rivHist[i]); }
    if (!isnan(FLOOD_ACTION)) mx = max(mx, FLOOD_ACTION);
    if (mx - mn < 0.5f) { float m = (mx + mn) / 2; mn = m - 0.25f; mx = m + 0.25f; }
    float pad = (mx - mn) * 0.12f; mn -= pad; mx += pad;
    auto yOf = [&](float v){ return (int)(Y1 - (v - mn) / (mx - mn) * (Y1 - Y0)); };
    display.drawFastVLine(X0 - 6, Y0, Y1 - Y0, GxEPD_BLACK);
    for (int g = 0; g <= 3; g++) {
      float v = mn + (mx - mn) * g / 3.0f; int gy = yOf(v);
      for (int x = X0; x < X1; x += 8) display.drawPixel(x, gy, GxEPD_BLACK);
      txtR(X0 - 10, gy + 5, String(v, 1), &FreeSans9pt7b);
    }
    if (!isnan(FLOOD_ACTION) && FLOOD_ACTION <= mx) {
      int fy = yOf(FLOOD_ACTION);
      display.drawFastHLine(X0, fy, X1 - X0, GxEPD_RED);
      txtR(X1, fy - 6, "action stage", &FreeSans9pt7b, GxEPD_RED);
    }
    int px = -1, py = -1;
    for (int i = 0; i < rivHistN; i++) {
      int gx = X0 + (int)((float)i / (rivHistN - 1) * (X1 - X0));
      int gy = yOf(rivHist[i]);
      if (px >= 0) { display.drawLine(px, py, gx, gy, GxEPD_BLACK);
                     display.drawLine(px, py + 1, gx, gy + 1, GxEPD_BLACK); }
      px = gx; py = gy;
    }
    display.fillCircle(px, py, 5, GxEPD_RED);
  } else txtC(SCR_W / 2, 370, "no history", &FreeSans12pt7b);

  txt(24, SCR_H - 14, String("USGS site ") + USGS_SITE + "    updated " + fmtTime(rivAt),
      &FreeSans9pt7b);
  if (isnan(FLOOD_ACTION))
    txtR(SCR_W - 24, SCR_H - 14, "flood stage not set", &FreeSans9pt7b, GxEPD_RED);
}
void drawQuakes() {
  time_t now; time(&now);
  display.fillScreen(GxEPD_WHITE);
  display.setTextWrap(false);
  txt(24, 52, "Earthquakes", &FreeSansBold24pt7b);
  display.setFont(&FreeSansBold24pt7b); display.setTextSize(1);
  txt(24 + textW("Earthquakes") + 24, 52, "UNITED STATES, M2.5+, PAST 24 HOURS - NEAREST FIRST", &FreeSansBold12pt7b);
  txtR(SCR_W - 24, 52, fmtTime(now, "%a %b %e   %l:%M %p"), &FreeSans12pt7b);
  display.fillRect(24, 68, SCR_W - 48, 3, GxEPD_BLACK);
  if (qkN == 0) { txtC(SCR_W / 2, 260, "No US quakes in the past 24 hours", &FreeSansBold18pt7b);
                  txt(24, SCR_H - 14, "earthquake.usgs.gov", &FreeSans9pt7b); return; }
  label(24, 96, "EVENTS");   txt(24, 134, String(qkTotal), &FreeSansBold18pt7b);
  label(150, 96, "LARGEST"); txt(150, 134, "M" + String(qkBig, 1), &FreeSansBold18pt7b,
                                 qkBig >= 6 ? GxEPD_RED : GxEPD_BLACK);
  label(300, 96, "NEAREST"); txt(300, 134, grpAbs(QK[0].dist) + " mi", &FreeSansBold18pt7b,
                                 QK[0].dist < 300 ? GxEPD_RED : GxEPD_BLACK);
  display.drawFastHLine(24, 152, SCR_W - 48, GxEPD_BLACK);
  int y = 186;
  for (int i = 0; i < qkN && y < 492; i++, y += 44) {
    uint16_t c = QK[i].mag >= 6 ? GxEPD_RED : GxEPD_BLACK;
    txt(24, y, "M" + String(QK[i].mag, 1), &FreeSansBold18pt7b, c);
    String pl[1]; wrapText(QK[i].place, &FreeSans12pt7b, 1, 520, pl, 1);
    txt(150, y, pl[0], &FreeSans12pt7b);
    txtR(730, y, grpAbs(QK[i].dist) + " mi", &FreeSans12pt7b,
         QK[i].dist < 300 ? GxEPD_RED : GxEPD_BLACK);
    if (QK[i].t) txtR(SCR_W - 24, y, agoStr(QK[i].t, now), &FreeSans9pt7b);
  }
  txt(24, SCR_H - 14, "earthquake.usgs.gov    updated " + fmtTime(qkAt), &FreeSans9pt7b);
}
void drawSpace() {
  time_t now; time(&now);
  display.fillScreen(GxEPD_WHITE);
  display.setTextWrap(false);
  txt(24, 52, "Space Weather", &FreeSansBold24pt7b);
  txtR(SCR_W - 24, 52, fmtTime(now, "%a %b %e   %l:%M %p"), &FreeSans12pt7b);
  display.fillRect(24, 68, SCR_W - 48, 3, GxEPD_BLACK);

  bool storm = !isnan(kpNow) && kpNow >= 5;
  label(24, 96, "PLANETARY Kp   0-9");
  String kv = isnan(kpNow) ? "--" : String(kpNow, 1);
  txt(20, 172, kv, &FreeSansBold24pt7b, storm ? GxEPD_RED : GxEPD_BLACK, 2);
  display.setFont(&FreeSansBold24pt7b); display.setTextSize(2);
  int kw = textW(kv); display.setTextSize(1);
  txt(20 + kw + 18, 172, kpBand(kpNow), &FreeSansBold18pt7b, storm ? GxEPD_RED : GxEPD_BLACK);
  // Northern Illinois sits near 42 N - aurora needs a severe storm to reach this far south.
  label(470, 96, "AURORA AT THIS LATITUDE");
  txt(470, 138, !isnan(kpNow) && kpNow >= 7 ? "POSSIBLE TONIGHT" : "Not likely",
      &FreeSansBold18pt7b, (!isnan(kpNow) && kpNow >= 7) ? GxEPD_RED : GxEPD_BLACK);
  txt(470, 176, "needs about Kp 7 to reach 42 N", &FreeSans9pt7b);
  display.fillRect(24, 214, SCR_W - 48, 2, GxEPD_BLACK);

  label(24, 240, "PLANETARY Kp, LAST 3 DAYS  (3-hourly)");
  const int X0 = 58, X1 = SCR_W - 24, Y0 = 258, Y1 = 452;
  auto yOf = [&](float v){ return (int)(Y1 - (v / 9.0f) * (Y1 - Y0)); };
  display.drawFastVLine(X0 - 6, Y0, Y1 - Y0, GxEPD_BLACK);
  for (int g = 0; g <= 9; g += 3) {
    int gy = yOf((float)g);
    for (int x = X0; x < X1; x += 8) display.drawPixel(x, gy, g >= 5 ? GxEPD_RED : GxEPD_BLACK);
    txtR(X0 - 10, gy + 5, String(g), &FreeSans9pt7b, g >= 5 ? GxEPD_RED : GxEPD_BLACK);
  }
  int sy = yOf(5);
  display.drawFastHLine(X0, sy, X1 - X0, GxEPD_RED);
  txtR(X1, sy - 6, "Kp 5 = storm", &FreeSans9pt7b, GxEPD_RED);
  display.drawFastHLine(X0, Y1, X1 - X0, GxEPD_BLACK);
  if (kpHistN > 0) {
    float step = (float)(X1 - X0) / (float)kpHistN;
    for (int i = 0; i < kpHistN; i++) {
      int bx = X0 + (int)(i * step) + 3, bw = (int)step - 6; if (bw < 3) bw = 3;
      int vy = yOf(kpHist[i]);
      display.fillRect(bx, vy, bw, Y1 - vy, kpHist[i] >= 5 ? GxEPD_RED : GxEPD_BLACK);
    }
    // x axis: tick + local day/hour under every 4th sample, i.e. every 12 h
    for (int i = 0; i < kpHistN; i += 4) {
      if (!kpTime[i]) continue;
      int cx = X0 + (int)(i * step) + (int)(step / 2);
      display.drawFastVLine(cx, Y1 + 1, 5, GxEPD_BLACK);
      struct tm lt; localtime_r(&kpTime[i], &lt);
      char d[8]; strftime(d, sizeof d, "%a", &lt);
      int hh = lt.tm_hour % 12; if (!hh) hh = 12;
      char lbl[16]; snprintf(lbl, sizeof lbl, "%s %d%c", d, hh, lt.tm_hour < 12 ? 'a' : 'p');
      txtC(cx, Y1 + 20, lbl, &FreeSans9pt7b);
    }
  }
  txt(24, SCR_H - 14, "services.swpc.noaa.gov    updated " + fmtTime(kpAt), &FreeSans9pt7b);
  txtR(SCR_W - 24, SCR_H - 14, "times are local", &FreeSans9pt7b);
}

// ---------------- air & allergy drawing ----------------
void drawAir() {
  time_t now; time(&now);
  display.fillScreen(GxEPD_WHITE);
  display.setTextWrap(false);
  txt(24, 52, "Air & Allergy", &FreeSansBold24pt7b);
  txtR(SCR_W - 24, 52, fmtTime(now, "%a %b %e   %l:%M %p"), &FreeSans12pt7b);
  display.fillRect(24, 68, SCR_W - 48, 3, GxEPD_BLACK);

  // ---- pollen, left ----
  bool pHot = !isnan(pollenIdx) && pollenIdx >= 7.3f;
  label(24, 96, "POLLEN TODAY   index 0-12");
  String pv = isnan(pollenIdx) ? "--" : f1(pollenIdx);
  txt(20, 168, pv, &FreeSansBold24pt7b, pHot ? GxEPD_RED : GxEPD_BLACK, 2);
  display.setFont(&FreeSansBold24pt7b); display.setTextSize(2);
  int pw = textW(pv); display.setTextSize(1);
  txt(20 + pw + 18, 168, pollenBand(pollenIdx), &FreeSansBold18pt7b, pHot ? GxEPD_RED : GxEPD_BLACK);
  label(24, 210, "TOP ALLERGENS");
  if (pollenTrig[0]) {
    String tl[2];
    int tn = wrapText(pollenTrig, &FreeSans12pt7b, 1, 400, tl, 2);
    for (int k = 0; k < tn; k++) txt(24, 242 + k * 26, tl[k], &FreeSans12pt7b);
  } else txt(24, 242, "--", &FreeSans12pt7b);

  display.drawFastVLine(448, 84, 210, GxEPD_BLACK);

  // ---- air quality, right ----
  bool aHot = aqiVal > 100;
  label(470, 96, "AIR QUALITY   US AQI");
  String av = aqiVal < 0 ? "--" : String(aqiVal);
  txt(466, 168, av, &FreeSansBold24pt7b, aHot ? GxEPD_RED : GxEPD_BLACK, 2);
  display.setFont(&FreeSansBold24pt7b); display.setTextSize(2);
  int aw = textW(av); display.setTextSize(1);
  if (aqiVal >= 0)
    txt(466 + aw + 18, 168, aqiBand(aqiVal), &FreeSansBold18pt7b, aHot ? GxEPD_RED : GxEPD_BLACK);
  statBlockSm(470, 210, "PM2.5 ug/m3", f1(pm25));
  statBlockSm(600, 210, "PM10 ug/m3",  f1(pm10));
  statBlockSm(730, 210, "OZONE ug/m3", f0(ozone));

  display.fillRect(24, 302, SCR_W - 48, 2, GxEPD_BLACK);

  // ---- asthma, lower left ----
  bool asHot = !isnan(asthmaIdx) && asthmaIdx >= 7.3f;
  label(24, 328, "ASTHMA INDEX");
  txt(20, 396, isnan(asthmaIdx) ? "--" : f1(asthmaIdx), &FreeSansBold24pt7b,
      asHot ? GxEPD_RED : GxEPD_BLACK, 2);
  txt(24, 430, pollenBand(asthmaIdx), &FreeSans12pt7b, asHot ? GxEPD_RED : GxEPD_BLACK);

  // ---- 5-day pollen forecast, lower right ----
  label(320, 328, "POLLEN FORECAST, NEXT 5 DAYS");
  const int X0 = 320, X1 = SCR_W - 24, Y0 = 348, Y1 = 462;
  float step = (float)(X1 - X0) / 5.0f;
  auto yOf = [&](float v){ return (int)(Y1 - (v / 12.0f) * (Y1 - Y0)); };
  int ty = yOf(7.3f);                                   // Med-High threshold
  for (int x = X0; x < X1; x += 10) display.drawFastHLine(x, ty, 5, GxEPD_RED);
  txtR(X1, ty - 6, "7.3 med-high", &FreeSans9pt7b, GxEPD_RED);
  display.drawFastHLine(X0, Y1, X1 - X0, GxEPD_BLACK);
  for (int i = 0; i < pollen5N; i++) {
    if (isnan(pollen5[i])) continue;
    int bx = X0 + (int)(i * step) + 16, bw = (int)step - 32;
    int vy = yOf(pollen5[i]);
    uint16_t c = pollen5[i] >= 7.3f ? GxEPD_RED : GxEPD_BLACK;
    display.fillRect(bx, vy, bw, Y1 - vy, c);
    txtC(bx + bw / 2, vy - 6, f1(pollen5[i]), &FreeSans9pt7b, c);
    char d[12]; snprintf(d, sizeof d, "%d/%d", p5mon[i], p5day[i]);
    txtC(bx + bw / 2, Y1 + 20, d, &FreeSans9pt7b);
  }

  txt(24, SCR_H - 14, String("pollen.com ZIP ") + POLLEN_ZIP +
      "    open-meteo air quality    updated " + fmtTime(airAt), &FreeSans9pt7b);
  txtR(SCR_W - 24, SCR_H - 14, "no mold data available for this area", &FreeSans9pt7b);
}

// ---------------- alert box ----------------
void drawAlertBox() {
  const int bx = 24, by = 346, bw = SCR_W - 48, bh = 146;
  int thick = (alertRank >= 2) ? 6 : 3;                  // tornado gets a heavier frame
  for (int i = 0; i < thick; i++) display.drawRect(bx + i, by + i, bw - 2 * i, bh - 2 * i, GxEPD_RED);
  String ln[2];
  int n = wrapText(alertEvent, &FreeSansBold24pt7b, 1, bw - 48, ln, 2);
  for (int k = 0; k < n; k++) txt(bx + 24, by + 50 + k * 40, ln[k], &FreeSansBold24pt7b, GxEPD_RED);
  int y = by + 50 + n * 40 + 26;
  if (y > by + bh - 12) y = by + bh - 12;
  txt(bx + 24, y, alertArea, &FreeSans12pt7b);
  if (alertEnds > 0) txtR(bx + bw - 24, y, "until " + fmtTime(alertEnds), &FreeSans12pt7b, GxEPD_RED);
}

// ---------------- forecast drawing ----------------
void drawForecast() {
  time_t now; time(&now);
  display.fillScreen(GxEPD_WHITE);
  display.setTextWrap(false);
  txt(24, 52, "Forecast", &FreeSansBold24pt7b);
  txtR(SCR_W - 24, 52, fmtTime(now, "%a %b %e   %l:%M %p"), &FreeSans12pt7b);
  display.fillRect(24, 68, SCR_W - 48, 3, GxEPD_BLACK);

  if (fcN == 0) {
    txtC(SCR_W / 2, 260, "Forecast unavailable", &FreeSansBold18pt7b, GxEPD_RED);
    txt(24, SCR_H - 14, "api.weather.gov", &FreeSans9pt7b);
    return;
  }

  const int COLW = 166, X0 = 24;
  for (int i = 0; i < fcN; i++) {
    int x = X0 + i * COLW;
    if (i) display.drawFastVLine(x - 10, 86, 230, GxEPD_BLACK);
    txt(x, 100, fc[i].name, &FreeSans9pt7b);
    String t = String(fc[i].temp);
    txt(x, 146, t, &FreeSansBold24pt7b);
    display.setFont(&FreeSansBold24pt7b); display.setTextSize(1);
    degSign(x + textW(t) + 9, 126, 5, GxEPD_BLACK);
    txt(x, 172, fc[i].day ? "HIGH" : "LOW", &FreeSans9pt7b);
    String ln[3];
    int n = wrapText(fc[i].shortF, &FreeSans9pt7b, 1, COLW - 18, ln, 3);
    for (int k = 0; k < n; k++) txt(x, 202 + k * 20, ln[k], &FreeSans9pt7b);
    if (fc[i].pop > 0)
      txt(x, 274, String(fc[i].pop) + "% precip", &FreeSans9pt7b,
          fc[i].pop >= 50 ? GxEPD_RED : GxEPD_BLACK);
    txt(x, 298, fc[i].wind, &FreeSans9pt7b);
  }
  display.fillRect(24, 322, SCR_W - 48, 2, GxEPD_BLACK);

  label(24, 348, fc[0].name);
  String dl[4];
  int dn = wrapText(fcDetail, &FreeSans12pt7b, 1, SCR_W - 48, dl, 4);
  for (int k = 0; k < dn; k++) txt(24, 378 + k * 26, dl[k], &FreeSans12pt7b);

  txt(24, SCR_H - 14, "api.weather.gov    updated " + fmtTime(fcAt), &FreeSans9pt7b);
  txtR(SCR_W - 24, SCR_H - 14, "Chicago, IL   grid LOT 76,73", &FreeSans9pt7b);
}

// ---------------- ComEd drawing ----------------
static String hourLab(int h){ int hh = h % 12; if (!hh) hh = 12; return String(hh) + (h < 12 ? "a" : "p"); }

void drawComEd() {
  time_t now; time(&now); struct tm lt; localtime_r(&now, &lt);
  display.fillScreen(GxEPD_WHITE);
  display.setTextWrap(false);
  txt(24, 52, "ComEd Hourly Pricing", &FreeSansBold24pt7b);
  txtR(SCR_W - 24, 52, fmtTime(now, "%a %b %e   %l:%M %p"), &FreeSans12pt7b);
  display.fillRect(24, 68, SCR_W - 48, 3, GxEPD_BLACK);

  float lo = 1e9, hi = -1e9, sum = 0; int n = 0, loH = 0, hiH = 0;
  for (int h = 0; h < 24; h++) if (!isnan(comedHour[h])) {
    if (comedHour[h] < lo) { lo = comedHour[h]; loH = h; }
    if (comedHour[h] > hi) { hi = comedHour[h]; hiH = h; }
    sum += comedHour[h]; n++;
  }
  float avg = n ? sum / n : NAN;

  bool curHigh = !isnan(comedCur) && comedCur >= ALERT_C;
  label(24, 96, "CURRENT HOUR AVERAGE");
  String cs = isnan(comedCur) ? "--" : f1(comedCur);
  txt(20, 180, cs, &FreeSansBold24pt7b, curHigh ? GxEPD_RED : GxEPD_BLACK, 2);
  display.setFont(&FreeSansBold24pt7b); display.setTextSize(2);
  int cx = 20 + textW(cs) + 16; display.setTextSize(1);
  txt(cx, 180, "c/kWh", &FreeSansBold18pt7b, curHigh ? GxEPD_RED : GxEPD_BLACK);

  statBlock(520, 96, "TODAY LOW", n ? f1(lo) + "c" : "--");
  statBlock(660, 96, "HIGH",      n ? f1(hi) + "c" : "--", (n && hi >= ALERT_C) ? GxEPD_RED : GxEPD_BLACK);
  statBlock(790, 96, "AVG",       n ? f1(avg) + "c" : "--");
  if (n) txt(520, 176, "low " + hourLab(loH) + "   high " + hourLab(hiH) + "   " + String(n) + " hrs so far", &FreeSans9pt7b);
  display.fillRect(24, 222, SCR_W - 48, 2, GxEPD_BLACK);

  // ---- 24-hour bar chart with a cents Y axis ----
  label(24, 246, "TODAY'S HOURLY AVERAGE  (cents per kWh)");
  const int AXW = 42;                                  // room for the Y-axis labels
  const int X0 = 24 + AXW, X1 = SCR_W - 24, Y0 = 258, Y1 = 462;

  float vtop = n ? hi : 0;
  if (!isnan(comedCur) && comedCur > vtop) vtop = comedCur;
  if (vtop < 9) vtop = 9;                              // keep the 8c line on screen on cheap days
  float vmax = vtop * 1.12f;
  float vmin = (n && lo < 0) ? lo * 1.15f : 0;
  float step = (float)(X1 - X0) / 24.0f;
  auto yOf = [&](float v){ return (int)(Y1 - (v - vmin) / (vmax - vmin) * (Y1 - Y0)); };
  int zeroY = yOf(0);

  // Y axis: pick a round tick step giving <= 6 gridlines
  const float CAND[] = {0.5f, 1, 2, 5, 10, 20, 50};
  float tstep = 50;
  for (float c : CAND) if ((vmax - vmin) / c <= 6) { tstep = c; break; }
  display.drawFastVLine(X0 - 6, Y0, Y1 - Y0 + 1, GxEPD_BLACK);
  for (float v = ceilf(vmin / tstep) * tstep; v <= vmax + 1e-3f; v += tstep) {
    int gy = yOf(v);
    if (gy < Y0 - 2 || gy > Y1 + 2) continue;
    bool hot = v >= ALERT_C - 1e-3f;                   // 8c and above: red
    uint16_t gc = hot ? GxEPD_RED : GxEPD_BLACK;
    for (int x = X0; x < X1; x += 8) display.drawPixel(x, gy, gc);
    display.drawFastHLine(X0 - 11, gy, 6, gc);
    txtR(X0 - 14, gy + 5, f0(v), &FreeSans9pt7b, gc);
  }
  display.drawFastHLine(X0 - 6, zeroY, X1 - X0 + 6, GxEPD_BLACK);

  for (int h = 0; h < 24; h++) {
    int bx = X0 + (int)(h * step) + 3, bw = (int)step - 6;
    bool inProgress = (h == lt.tm_hour && isnan(comedHour[h]) && !isnan(comedCur));
    float v = inProgress ? comedCur : comedHour[h];
    if (isnan(v)) continue;
    int vy = yOf(v);
    uint16_t c = v >= ALERT_C ? GxEPD_RED : GxEPD_BLACK;
    int top = v >= 0 ? vy : zeroY, hgt = v >= 0 ? zeroY - vy : vy - zeroY;
    if (hgt < 2) hgt = 2;
    if (inProgress) display.drawRect(bx, top, bw, hgt, c);   // hollow = hour still filling
    else            display.fillRect(bx, top, bw, hgt, c);
    if (h % 3 == 0) txtC(bx + bw / 2, 484, hourLab(h), &FreeSans9pt7b);
  }

  // the 8c alert threshold, drawn solid on top of its gridline
  if (ALERT_C >= vmin && ALERT_C <= vmax) {
    int ay = yOf(ALERT_C);
    display.drawFastHLine(X0, ay,     X1 - X0, GxEPD_RED);
    display.drawFastHLine(X0, ay + 1, X1 - X0, GxEPD_RED);
    txtR(X1 - 4, ay - 7, "8c alert", &FreeSans9pt7b, GxEPD_RED);
  }

  String foot = String("Updated ") + fmtTime(comedAt) + "   hourlypricing.comed.com   " + WiFi.localIP().toString();
  txt(24, SCR_H - 14, foot, &FreeSans9pt7b);
  txtR(SCR_W - 24, SCR_H - 14, "hollow bar = hour in progress", &FreeSans9pt7b);
}

static bool lastRenderOk = true;
void drawPage() {
  display.fillScreen(GxEPD_WHITE);
  display.setTextWrap(false);          // never wrap: an overflowing label would orphan chars at x=0
  time_t now; time(&now);

  // ---- header ----
  txt(24, 52, STATION_NAME, &FreeSansBold24pt7b);
  txtR(SCR_W - 24, 52, fmtTime(now, "%a %b %e   %l:%M %p"), &FreeSans12pt7b);
  display.fillRect(24, 68, SCR_W - 48, 3, GxEPD_BLACK);
  if (msgText.length() && msgFull) { drawFullMessage(now); return; }

  // ---- left: temperature ----
  uint16_t tcol = (!isnan(cur.tempf) && (cur.tempf <= 32 || cur.tempf >= 90)) ? GxEPD_RED : GxEPD_BLACK;
  label(24, 104, "TEMPERATURE");
  String ts = f0(cur.tempf);
  txt(20, 226, ts, &FreeSansBold24pt7b, tcol, 3);              // ~100 px digits
  display.setFont(&FreeSansBold24pt7b); display.setTextSize(3);
  int tx = 20 + textW(ts) + 14;
  degSign(tx + 10, 140, 9, tcol);
  txt(tx + 26, 172, "F", &FreeSansBold24pt7b, tcol);
  display.setTextSize(1);
  hiLoBlock(300, 122, "HIGH TODAY", dayHi, dayHiT);
  hiLoBlock(300, 202, "LOW TODAY",  dayLo, dayLoT);

  // AWN's feelsLike IS wind chill when cold and heat index when hot - name it accordingly.
  const char* flLab = "FEELS LIKE";
  if (!isnan(cur.tempf)) {
    if (cur.tempf <= 50 && !isnan(cur.windMph) && cur.windMph >= 3) flLab = "WIND CHILL";
    else if (cur.tempf >= 80)                                       flLab = "HEAT INDEX";
  }
  statBlock(24,  290, flLab, f0(cur.feelsLike) + "~");
  statBlock(174, 290, "DEW POINT",  f0(cur.dewPoint) + "~");
  statBlock(324, 290, "HUMIDITY",   f0(cur.humidity, "%"));

  // ---- divider ----
  display.fillRect(448, 92, 2, 250, GxEPD_BLACK);

  // ---- right: pressure ----
  label(474, 104, "BAROMETRIC PRESSURE  (relative, mb)");
  txt(470, 196, f1(cur.baromRel), &FreeSansBold24pt7b, GxEPD_BLACK, 2);   // ~66 px digits
  display.setFont(&FreeSansBold24pt7b); display.setTextSize(2);
  int pw = textW(f1(cur.baromRel)); display.setTextSize(1);
  txt(470 + pw + 12, 196, "mb", &FreeSansBold18pt7b);
  // trend line under the big number: arrow at left, then word + delta. Keeps clear of the digits.
  bool falling = !isnan(trendDelta) && trendDelta <= -TREND_MB;
  uint16_t trc = falling ? GxEPD_RED : GxEPD_BLACK;
  drawArrow(494, 236, trendDelta, trc);
  if (!isnan(trendDelta)) {
    String d = (trendDelta > 0 ? "+" : "") + f1(trendDelta) + " mb / " + String(trendSpanMin / 60) + "h";
    txt(530, 226, falling ? "FALLING" : (trendDelta >= TREND_MB ? "RISING" : "STEADY"), &FreeSans9pt7b, trc);
    txt(530, 254, d, &FreeSansBold18pt7b, trc);
  }
  // Direction lives in the LABEL: appending it to the value overflowed into GUST,
  // and the third block ran off the right edge. Columns re-spaced to 474 / 610 / 745.
  String wd = cardinal(isnan(cur.windDir) ? cur.windDirAvg : cur.windDir);
  // Equal thirds of the 474..856 span (127 px each). "WIND FROM NNE" needed 136 px and forced
  // the other two columns right, squeezing GUST; the short form fits and keeps the row even.
  txt(474, 290, wd.length() ? "WIND " + wd : String("WIND"), &FreeSans9pt7b);
  txt(474, 324, f0(cur.windMph, " mph"), &FreeSansBold18pt7b);
  statBlock(601, 290, "GUST",      f0(cur.windGust, " mph"));
  statBlock(728, 290, "MAX TODAY", f0(cur.maxGust, " mph"));
  display.drawFastHLine(24, 336, SCR_W - 48, GxEPD_BLACK);

  if (alertLive()) drawAlertBox();
  else if (msgText.length()) drawBannerMessage();
  else {
    // ---- conditions row: UV, solar, sun times, indoor ----
    double sr = NAN, ss = NAN;
    { struct tm lt; localtime_r(&now, &lt);
      sunTimes(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, SITE_LAT, SITE_LON, tzOffsetSec(now), sr, ss); }
    bool uvHigh = !isnan(cur.uv) && cur.uv >= 6;
    statBlockSm( 24, 356, "UV INDEX",   f0(cur.uv),               uvHigh ? GxEPD_RED : GxEPD_BLACK);
    statBlockSm(164, 356, "SOLAR W/m2", f0(cur.solar));
    statBlockSm(304, 356, "SUNRISE",    fmtMin(sr));
    statBlockSm(444, 356, "SUNSET",     fmtMin(ss));
    statBlockSm(584, 356, "DAYLIGHT",   (isnan(sr) || isnan(ss)) ? "--"
                 : String((int)((ss - sr)) / 60) + "h " + String((int)(ss - sr) % 60) + "m");
    statBlockSm(724, 356, "INDOOR",     f0(cur.tempIn) + "F  " + f0(cur.humIn, "%"));

    // ---- rain line ----
    String rain = "RAIN    hour " + f1(cur.rainHour, "\"") + "    today " + f1(cur.dailyRain, "\"") +
                  "    week " + f1(cur.rainWeek, "\"") + "    month " + f1(cur.rainMonth, "\"") +
                  "    year " + f1(cur.rainYear, "\"") + "    last rain " + agoStr(cur.lastRainT, now);
    txt(24, 418, rain, &FreeSans9pt7b);

    // ---- lightning row ----
    // Red when there are strikes in the past hour. Prefer the station's own hourly
    // counter; fall back to the last-strike timestamp if that field is missing.
    bool ltgRecent = (!isnan(cur.ltgHour) && cur.ltgHour > 0) ||
                     (cur.ltgTime > 0 && (now - cur.ltgTime) < 3600);
    bool ltgToday  = !isnan(cur.ltgDay) && cur.ltgDay > 0;
    uint16_t lcol = ltgRecent ? GxEPD_RED : GxEPD_BLACK;
    drawBolt(24, 434, lcol);
    String ls;
    if (!ltgToday && cur.ltgTime == 0) ls = "No lightning today";
    else {
      ls = f0(cur.ltgDay) + (cur.ltgDay == 1 ? " strike today" : " strikes today");
      if (cur.ltgTime > 0) ls += "   last " + agoStr(cur.ltgTime, now);
      if (!isnan(cur.ltgDist)) ls += "   " + f0(cur.ltgDist) + " mi";
    }
    txt(52, 456, ls, &FreeSans12pt7b, lcol);

  }

  // ---- footer ----
  String foot = "Obs " + fmtTime(cur.t) + "     Updated " + fmtTime(lastGood) +
                "     " + String(fetchOk) + "/" + String(fetchTotal) + " ok     " +
                WiFi.localIP().toString() + "  " + String(WiFi.RSSI()) + " dBm";
  txt(24, SCR_H - 14, foot, &FreeSans9pt7b);
  if (!lastRenderOk) txtR(SCR_W - 24, SCR_H - 14, "FETCH ERROR: " + lastErr, &FreeSans9pt7b, GxEPD_RED);
}

void render(bool ok) {
  lastRenderOk = ok;
  uint32_t t0 = millis();
  display.setFullWindow();
  display.setRotation(0);
  display.firstPage();
  do { bool msgOwns = msgText.length() && msgFull;
       if      (curScreen == SCR_PORTFOLIO && !msgOwns) drawPortfolio();
       else if (curScreen == SCR_COMED     && !msgOwns) drawComEd();
       else if (curScreen == SCR_FORECAST  && !msgOwns) drawForecast();
       else if (curScreen == SCR_NEWS      && !msgOwns) drawNews();
       else if (curScreen == SCR_AIR       && !msgOwns) drawAir();
       else if (curScreen == SCR_RIVER     && !msgOwns) drawRiver();
       else if (curScreen == SCR_QUAKE     && !msgOwns) drawQuakes();
       else if (curScreen == SCR_SPACE     && !msgOwns) drawSpace();
       else                                             drawPage(); } while (display.nextPage());
  display.hibernate();
  Serial.printf("render done in %lu ms\n", (unsigned long)(millis() - t0));
}
void renderMessage(const String& big, const String& small, uint16_t col = GxEPD_BLACK) {
  display.setFullWindow(); display.setRotation(0);
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    txt(24, 52, STATION_NAME, &FreeSansBold24pt7b);
    display.fillRect(24, 68, SCR_W - 48, 3, GxEPD_BLACK);
    txtC(SCR_W / 2, SCR_H / 2, big, &FreeSansBold24pt7b, col, 2);
    txtC(SCR_W / 2, SCR_H / 2 + 60, small, &FreeSans12pt7b);
  } while (display.nextPage());
  display.hibernate();
}

// Only redraw when what the screen shows has changed (a full refresh is ~23 s of flashing).
static String signature() {
  char b[160];
  snprintf(b, sizeof b, "%ld|%.0f|%.1f|%.0f|%.0f|%.0f|%.0f|%.0f|%.1f|%.1f|%.0f|%ld|%d",
           (long)cur.t, cur.tempf, cur.baromRel, cur.humidity, cur.feelsLike, cur.dewPoint,
           cur.windMph, cur.windGust, cur.dailyRain, trendDelta, cur.ltgDay, (long)cur.ltgTime, histN);
  return String(b);
}
static String lastSig;


// ---------------- web UI / API ----------------
static const char FORM_HTML[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>%NAME% e-paper</title><style>body{font-family:sans-serif;max-width:520px;margin:2em auto;padding:0 1em;color:#222}
textarea,input,select,button{width:100%;font-size:1.05em;margin:.35em 0;box-sizing:border-box;padding:.5em}button{background:#b00;color:#fff;border:0;border-radius:6px}
.c button{background:#666}p.s{background:#f3f3f3;padding:.7em;border-radius:6px}</style></head><body>
<h2>%NAME% e-paper</h2><p class=s>%STATUS%</p>
<form method=post action=/message><textarea name=text rows=4 placeholder="Message text (Enter = new line; *asterisks* make words red)"></textarea>
<select name=style><option value=banner>Banner under the weather</option><option value=full>Full screen</option></select>
<small>Tip: wrap words in *asterisks* to print them in red.</small><label>Minutes to show (0 = until cleared)</label><input name=ttl type=number value=60 min=0>
<button>Show on e-paper (takes ~25 s)</button></form>
<form class=c method=post action=/clear><button>Clear message</button></form>
<h3>Severe weather test</h3>
<form method=post action=/testalert><input type=hidden name=event value="Severe Thunderstorm Warning"><button>Test thunderstorm box</button></form>
<form method=post action=/testalert><input type=hidden name=event value="Tornado Warning"><button>Test tornado box</button></form>
<form class=c method=post action=/alertclear><button>Clear test alert</button></form>
<h3>Screens</h3>
<form method=post action=/screen><input type=hidden name=s value=weather><button>Show weather now</button></form>
<form method=post action=/screen><input type=hidden name=s value=portfolio><button>Show portfolio now</button></form>
<form method=post action=/screen><input type=hidden name=s value=comed><button>Show ComEd prices now</button></form>
<form method=post action=/screen><input type=hidden name=s value=forecast><button>Show forecast now</button></form>
<form method=post action=/screen><input type=hidden name=s value=news><button>Show WSJ headlines now</button></form>
<form method=post action=/screen><input type=hidden name=s value=air><button>Show air &amp; allergy now</button></form>
<form method=post action=/screen><input type=hidden name=s value=river><button>Show river gauge now</button></form>
<form method=post action=/screen><input type=hidden name=s value=space><button>Show space weather now</button></form>
<form method=post action=/screen><input type=hidden name=s value=quake><button>Show earthquakes now</button></form>
<form method=post action=/rotate><label>Rotate every N minutes (0 = off)</label><input name=min type=number value=3 min=0><button>Set rotation</button></form></body></html>)HTML";

static const char* screenName(Screen v) {
  switch (v) {
    case SCR_FORECAST:  return "forecast";
    case SCR_AIR:       return "air";
    case SCR_RIVER:     return "river";
    case SCR_SPACE:     return "space";
    case SCR_QUAKE:     return "quake";
    case SCR_PORTFOLIO: return "portfolio";
    case SCR_COMED:     return "comed";
    case SCR_NEWS:      return "news";
    default:            return "weather";
  }
}
static String statusLine() {
  String base = String("Screen: ") + (screenName(curScreen)) +
                (rotateMs ? "  (rotating every " + String(rotateMs / 60000) + " min, weather every 3rd)" : "  (rotation off)") +
                "  |  quotes " + String(quotesOk) + "/" + String(NHOLD) +
                "  |  today hi " + (isnan(dayHi) ? String("--") : f1(dayHi) + " at " + fmtTime(dayHiT)) +
                ", lo " + (isnan(dayLo) ? String("--") : f1(dayLo) + " at " + fmtTime(dayLoT)) +
                (alertLive() ? String("  |  *** ") + alertEvent + " (" + alertArea + ") until " + fmtTime(alertEnds)
                               + (alertTest ? " [TEST]" : "") + " ***" : String("  |  no alerts")) + ". ";
  if (!msgText.length()) return base + "No message showing.";
  return base + "Showing (" + (msgFull ? "full screen" : "banner") + ", " + msgExpiryText() + ", rotation paused): " + msgText;
}
static void wantsHtmlRedirect() {          // browser form -> back to the page; curl -> plain text
  if (server.header("Accept").indexOf("text/html") >= 0) { server.sendHeader("Location", "/"); server.send(303); }
  else server.send(200, "text/plain", statusLine() + "\n");
}
static void handleRoot() {
  String h = FPSTR(FORM_HTML); h.replace("%NAME%", STATION_NAME); h.replace("%STATUS%", statusLine());
  server.send(200, "text/html", h);
}
static void handleMessage() {
  String t = server.arg("text"); t.trim();
  if (!t.length()) { server.send(400, "text/plain", "missing text\n"); return; }
  if (t.length() > 400) t = t.substring(0, 400);
  msgText  = t;
  msgFull  = server.arg("style") == "full";
  long ttl = server.hasArg("ttl") ? server.arg("ttl").toInt() : 60;
  msgTtlMs = ttl > 0 ? (uint32_t)ttl * 60000UL : 0;
  msgSetMs = millis();
  if (!msgFull) curScreen = SCR_WEATHER;   // the banner strip is part of the weather layout
  needRender = true;
  Serial.printf("message set (%s, ttl %ld min): %s\n", msgFull ? "full" : "banner", ttl, t.c_str());
  wantsHtmlRedirect();
}
static void handleClear() {
  if (msgText.length()) { msgText = ""; needRender = true; Serial.println("message cleared"); }
  wantsHtmlRedirect();
}
static void startWeb() {
  const char* hdrs[] = {"Accept"}; server.collectHeaders(hdrs, 1);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/message", handleMessage);
  server.on("/clear", handleClear);
  server.on("/alertclear", [](){ alertTest = false; alertActive = false; needRender = true; wantsHtmlRedirect(); });
  server.on("/status", HTTP_GET, [](){ server.send(200, "text/plain", statusLine() + "\n"); });
  server.on("/screen", [](){                       // /screen?s=weather|portfolio  - show one now
    String w = server.arg("s");
    if      (w == "portfolio") curScreen = SCR_PORTFOLIO;
    else if (w == "comed")     curScreen = SCR_COMED;
    else if (w == "forecast")  curScreen = SCR_FORECAST;
    else if (w == "news")      curScreen = SCR_NEWS;
    else if (w == "air")       curScreen = SCR_AIR;
    else if (w == "river")     curScreen = SCR_RIVER;
    else if (w == "quake")     curScreen = SCR_QUAKE;
    else if (w == "space")     curScreen = SCR_SPACE;
    else if (w == "weather")   curScreen = SCR_WEATHER;
    else { server.send(400, "text/plain", "s=weather|forecast|air|river|space|quake|portfolio|comed|news\n"); return; }
    lastRotate = millis(); needRender = true; wantsHtmlRedirect();
  });
  server.on("/testalert", [](){      // verify the red box without waiting for a storm
    String ev = server.hasArg("event") ? server.arg("event") : String("Severe Thunderstorm Warning");
    long m = server.hasArg("min") ? server.arg("min").toInt() : 10;
    strlcpy(alertEvent, ev.c_str(), sizeof alertEvent);
    strlcpy(alertArea, (server.hasArg("area") ? server.arg("area") : String("Lake, IL  (TEST)")).c_str(), sizeof alertArea);
    time_t n; time(&n); alertEnds = n + (m > 0 ? m : 10) * 60;
    alertRank = strstr(alertEvent, "Tornado") ? 2 : 1;
    alertActive = true; alertTest = true; curScreen = SCR_WEATHER; needRender = true;
    Serial.printf("TEST alert: %s for %ld min\n", alertEvent, m);
    wantsHtmlRedirect();
  });
  server.on("/rotate", [](){                       // /rotate?min=N  - 0 turns rotation off
    if (!server.hasArg("min")) { server.send(400, "text/plain", "min=<minutes, 0=off>\n"); return; }
    long m = server.arg("min").toInt();
    rotateMs = m > 0 ? (uint32_t)m * 60000UL : 0;
    lastRotate = millis();
    Serial.printf("rotation set to %ld min\n", m);
    wantsHtmlRedirect();
  });
  server.begin();
  if (MDNS.begin("epaper")) { MDNS.addService("http", "tcp", 80); Serial.println("mDNS: http://epaper.local"); }
}

// ---------------- WiFi (robust retry for WPA2/WPA3-mixed on WROOM-32) ----------------
bool connectWiFi(uint32_t overall_ms) {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  uint32_t start = millis();
  int attempt = 0;
  while (millis() - start < overall_ms) {
    attempt++;
    WiFi.disconnect(true, true);
    delay(250);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    uint32_t a0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - a0 < 8000) delay(150);
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("WiFi connected as %s (RSSI %d, try %d)\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI(), attempt);
      return true;
    }
    Serial.printf("WiFi try %d failed (status %d)\n", attempt, WiFi.status());
  }
  return false;
}

// ---------------- setup / loop ----------------
uint32_t lastCurrent = 0, lastHistory = 0;
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nWeatherEpaper starting (GxEPD2_750c_Z90 880x528)");

  for (int i = 0; i < 24; i++) comedHour[i] = NAN;   // zero-init would draw 24 bogus 0.0c bars

  display.init(115200, true, 2, false);
  SPI.end();
  SPI.begin(13, 12, 14, 15);          // Waveshare driver-board SPI pins (after init)

  bool wifi = connectWiFi(60000);
  configTzTime("CST6CDT,M3.2.0/2,M11.1.0/2", "pool.ntp.org", "time.nist.gov");
  if (wifi) {
    startWeb();
    uint32_t t0 = millis();                 // give NTP up to 8 s so "Updated" stamps are real
    while (time(nullptr) < 1000000000 && millis() - t0 < 8000) delay(100);
    Serial.printf("NTP %s after %lu ms\n", time(nullptr) < 1000000000 ? "NOT synced" : "synced", (unsigned long)(millis() - t0));
  }

  if (wifi) {
    tryN(fetchHistory, "history"); lastHistory = millis();
    bool ok = tryN(fetchCurrent, "current"); lastCurrent = millis();
    backfillDayHiLo();
    fetchQuotes(); lastQuotes = millis();
    fetchComEd();  lastComed  = millis();
    delay(400); fetchForecast(); lastFc = millis();
    delay(400); fetchAlerts();   lastAlert = millis();
    delay(400); fetchNews();     lastNews  = millis();
    delay(400); fetchAir();      lastAir   = millis();
    delay(400); tryN(fetchRiver,  "river");  lastRiver = millis();
    delay(400); tryN(fetchQuakes, "quakes"); lastQuake = millis();
    delay(400); tryN(fetchSpace,  "space");  lastSpace = millis();
    lastSig = signature(); lastRotate = millis();
    render(ok);
  } else {
    renderMessage("No Wi-Fi", String("Could not join ") + WIFI_SSID + " after 60 s - retrying", GxEPD_RED);
  }
}
void loop() {
  if (WiFi.status() != WL_CONNECTED) { if (connectWiFi(60000)) { lastCurrent = 0; } return; }
  server.handleClient();
  uint32_t now = millis();
  if (msgText.length() && msgTtlMs && now - msgSetMs >= msgTtlMs) { msgText = ""; needRender = true; Serial.println("message expired"); }
  if (needRender) { needRender = false; lastSig = signature(); render(lastRenderOk); }
  if (now - lastHistory >= HISTORY_MS) { tryN(fetchHistory, "history"); lastHistory = now; }
  if (now - lastQuotes  >= QUOTES_MS)  { fetchQuotes();                  lastQuotes  = now; }
  if (now - lastComed   >= COMED_MS)   { fetchComEd();                   lastComed   = now; }
  if (now - lastFc      >= FC_MS)      { fetchForecast();                lastFc      = now; }
  if (alertTest && !alertLive()) { alertTest = false; alertActive = false; needRender = true; Serial.println("test alert expired"); }
  if (!alertTest && now - lastAlert >= ALERT_MS) { fetchAlerts(); lastAlert = now; }
  if (now - lastNews >= NEWS_MS) { fetchNews(); lastNews = now; }
  if (now - lastAir   >= AIR_MS)   { fetchAir();    lastAir   = now; }
  if (now - lastRiver >= RIVER_MS) { tryN(fetchRiver,  "river");  lastRiver = now; }
  if (now - lastQuake >= QUAKE_MS) { tryN(fetchQuakes, "quakes"); lastQuake = now; }
  if (now - lastSpace >= SPACE_MS) { tryN(fetchSpace,  "space");  lastSpace = now; }
  if (now - lastCurrent >= CURRENT_MS) {
    bool ok = tryN(fetchCurrent, "current"); lastCurrent = now;
    String sig = signature();
    // Only repaint for new weather data if the weather screen is actually the one showing.
    if (curScreen == SCR_WEATHER && !msgText.length() && (sig != lastSig || ok != lastRenderOk)) {
      lastSig = sig; render(ok);
    } else { lastSig = sig; lastRenderOk = ok; }
  }
  { time_t nw; time(&nw); struct tm lt; localtime_r(&nw, &lt);
    if (dayKey >= 0 && lt.tm_yday != dayKey) { Serial.println("new day - refreshing hi/lo"); backfillDayHiLo(); } }
  // Rotation: paused while a message is up, so the message stays put for its whole TTL.
  if (rotateMs && !msgText.length() && !alertLive() && now - lastRotate >= rotateMs) {
    rotSlot++;
    if (rotSlot % 3 == 0) curScreen = SCR_WEATHER;
    else { curScreen = OTHERS[otherIdx]; otherIdx = (otherIdx + 1) % NOTHER; }
    lastRotate = now;
    Serial.printf("rotate -> %s\n", screenName(curScreen));
    render(lastRenderOk);
  }
  delay(200);
}
