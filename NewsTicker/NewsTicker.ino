// News & Markets: top U.S. headlines plus a row of stock quotes on the 4-color Imagotag/Vusion
// E300 7.4" panel (Pervasive Displays E2741QS0B3, Spectra 4 black/white/RED/YELLOW, 480x800
// native, used LANDSCAPE 800x480 via bb_epaper rotation 90). The board pulls everything itself
// over Wi-Fi; nothing runs on a host.
//
// Built from ComEdPanel4C (paged band renderer, Wi-Fi retry, board block for the XIAO EE05) with
// the Google News parser from WeatherPanel4C and the keyless Yahoo chart-API quote fetch from the
// pre-27-Oct WeatherPanel4C portfolio screen.
//
//   arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3_Plus:PSRAM=opi,PartitionScheme=app3M_fat9M_16MB \
//     --library bb_epaper_git NewsTicker
//
// If a fetch fails the last good headlines/prices stay up and a red "stale" line appears.

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <bb_epaper.h>
#include <Adafruit_GFX.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include "secrets.h"

// ================= settings =================
#define SCREEN_TITLE "NEWS & MARKETS"
// Google News top stories, United States. Alternative (one outlet):
//   "https://news.google.com/rss/search?q=site:wsj.com+when:2d&hl=en-US&gl=US&ceid=US:en"
const char* NEWS_URL = "https://news.google.com/rss?hl=en-US&gl=US&ceid=US:en";
// Yahoo symbols ('^' must be written %5E) and the label shown. Up to 8, four per row.
struct Ticker { const char* sym; const char* label; float price; float prev; };
Ticker TICKERS[] = {
  {"%5EGSPC", "S&P 500", NAN, NAN},
  {"%5EDJI",  "Dow",     NAN, NAN},
  {"%5EIXIC", "Nasdaq",  NAN, NAN},
  {"AAPL",    "AAPL",    NAN, NAN},
  {"NVDA",    "NVDA",    NAN, NAN},
  {"MSFT",    "MSFT",    NAN, NAN},
};
const int REFRESH_MINUTES = 10;
const int FRESH_MINUTES   = 30;          // headlines newer than this get a red marker
const int MAX_LINES       = 2;           // wrapped lines per headline
#define TIME_ZONE "CST6CDT,M3.2.0/2,M11.1.0/2"
// ============================================

#if defined(ARDUINO_XIAO_ESP32S3_PLUS) || defined(ARDUINO_XIAO_ESP32S3)
// Seeed XIAO ePaper Display Board EE05 V1.1x + XIAO ESP32-S3 Plus (also fine on the EE04,
// where GPIO43 is unused).
#define DC_PIN    10
#define RESET_PIN 38
#define BUSY_PIN  4
#define CS_PIN    44
#define SCK_PIN   7
#define MOSI_PIN  9
#define PWR_EN_PIN 43         // EE05: HIGH = panel powered
#define HOSTNAME  "news-panel-s3"
#else
// Waveshare e-Paper ESP32 Driver Board Rev3 (ESP32-WROOM-32E, no PSRAM).
#define DC_PIN    27
#define RESET_PIN 26
#define BUSY_PIN  25
#define CS_PIN    15
#define SCK_PIN   13
#define MOSI_PIN  14
#define HOSTNAME  "news-panel"
#endif
#define ROTATION  90
#define SCR_W 800
#define SCR_H 480

#define C_BLACK  BBEP_BLACK
#define C_WHITE  BBEP_WHITE
#define C_YELLOW BBEP_YELLOW
#define C_RED    BBEP_RED

BBEPAPER bbep(EP74YR_480x800);

// Adafruit_GFX -> panel bridge, paged in 80-column bands (see ComEdPanel4C).
const int BAND_W = 80, ROW_B = SCR_H / 4;
static uint8_t* band = nullptr;
static int bandX0 = 0;
class EpdGfx : public Adafruit_GFX {
 public:
  EpdGfx() : Adafruit_GFX(SCR_W, SCR_H) {}
  void drawPixel(int16_t x, int16_t y, uint16_t c) override {
    if (y < 0 || y >= SCR_H || x < bandX0 || x >= bandX0 + BAND_W) return;
    int nx = SCR_H - 1 - y;
    uint8_t* p = band + (x - bandX0) * ROW_B + (nx >> 2);
    int sh = (3 - (nx & 3)) * 2;
    *p = (*p & ~(3 << sh)) | ((c & 3) << sh);
  }
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) override {
    int x1 = max((int)x, bandX0), x2 = min((int)x + w - 1, bandX0 + BAND_W - 1);
    int y1 = max((int)y, 0),      y2 = min((int)y + h - 1, SCR_H - 1);
    for (int yy = y1; yy <= y2; yy++) for (int xx = x1; xx <= x2; xx++) drawPixel(xx, yy, c);
  }
  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t c) override { fillRect(x, y, w, 1, c); }
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t c) override { fillRect(x, y, 1, h, c); }
  void fillScreen(uint16_t c) override { uint8_t v = c & 3; memset(band, v | v << 2 | v << 4 | v << 6, BAND_W * ROW_B); }
};
EpdGfx display;
static bool panelAsleep = false;

// ---------------- data ----------------
struct Story { char title[160]; char source[40]; time_t t; };
const int NEWS_MAX = 8;
Story  NEWS[NEWS_MAX];
int    newsN = 0;
bool   newsOk = false;
const int NTICK = sizeof(TICKERS) / sizeof(TICKERS[0]);
bool   quotesOk = false;
time_t lastGood = 0;
String lastErr;

// ---------------- helpers ----------------
static String fmtTime(time_t t, const char* fmt = "%l:%M %p") {
  if (t < 1000000000) return "--:--";
  struct tm tm; localtime_r(&t, &tm);
  char buf[40]; strftime(buf, sizeof buf, fmt, &tm);
  String s(buf); s.trim(); return s;
}
static String agoStr(time_t then, time_t now) {
  if (then <= 0) return "";
  long s = now - then; if (s < 0) s = 0;
  if (s < 3600)  return String(s / 60) + " min ago";
  if (s < 86400) return String(s / 3600) + " hr ago";
  return String(s / 86400) + " d ago";
}
static String fmtPrice(float v) {          // 52310.11 -> "52,310.11"
  if (isnan(v)) return "--";
  char b[24]; snprintf(b, sizeof b, "%.2f", v);
  String s(b); int dot = s.indexOf('.');
  for (int i = dot - 3; i > 0; i -= 3) s = s.substring(0, i) + "," + s.substring(i);
  return s;
}

static const size_t BODY_CAP = 40000;
static char* bodyBuf = nullptr;            // PSRAM when available
static bool httpGetRaw(const String& url, size_t& outLen, size_t maxBytes = BODY_CAP - 1) {
  uint32_t t0 = millis();
  WiFiClientSecure client; client.setInsecure();
  HTTPClient https;
  https.useHTTP10(true);
  https.setTimeout(15000);
  https.setUserAgent("Mozilla/5.0 (X11; Linux x86_64)");   // Yahoo 404s the default ESP32 UA
  if (!https.begin(client, url)) { lastErr = "begin failed"; return false; }
  int code = https.GET();
  if (code != HTTP_CODE_OK) {
    lastErr = "HTTP " + String(code);
    Serial.printf("  GET %s FAILED: %s\n", url.substring(0, 50).c_str(), lastErr.c_str());
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
  Serial.printf("  GET %s -> %u bytes in %lu ms (heap %u)\n", url.substring(0, 50).c_str(),
                (unsigned)len, (unsigned long)(millis() - t0), (unsigned)ESP.getFreeHeap());
  return len > 0;
}

// UTF-8 -> the 7-bit range the Adafruit fonts cover, plus the XML entities Google News emits.
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
    if (c == 0xC2 && (unsigned char)s[i + 1] == 0xA0) { d[o++] = ' '; i += 2; continue; }  // nbsp
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
  return (time_t)(days * 86400L + h * 3600L + mi * 60L + se);
}

// ---------------- fetches ----------------
// The top-stories feed is ~120 KB; the first 8 items end inside ~29 KB, so read 36 KB and hang up.
bool fetchNews() {
  size_t len;
  if (!httpGetRaw(NEWS_URL, len, 36000)) return false;
  Story tmp[NEWS_MAX];
  int n = 0;
  const char* p = bodyBuf;
  while (n < NEWS_MAX && (p = strstr(p, "<item>")) != nullptr) {
    const char* end = strstr(p, "</item>");
    if (!end) break;                                             // truncated mid-item
    const char* t  = strstr(p, "<title>");
    const char* te = t ? strstr(t, "</title>") : nullptr;
    if (!t || !te || te > end) { p = end + 7; continue; }
    char raw[320];
    size_t l = (size_t)(te - (t + 7)); if (l >= sizeof raw) l = sizeof raw - 1;
    memcpy(raw, t + 7, l); raw[l] = 0;
    char clean[200];
    asciiClean(clean, sizeof clean, raw);
    // Google News appends " - <outlet>"; split at the LAST separator.
    tmp[n].source[0] = 0;
    for (char* q = clean + strlen(clean) - 3; q > clean; q--)
      if (q[0] == ' ' && q[1] == '-' && q[2] == ' ') { strlcpy(tmp[n].source, q + 3, sizeof tmp[n].source); *q = 0; break; }
    strlcpy(tmp[n].title, clean, sizeof tmp[n].title);
    const char* d = strstr(p, "<pubDate>");
    tmp[n].t = (d && d < end) ? parseRFC822(d + 9) : 0;
    if (tmp[n].title[0]) n++;
    p = end + 7;
  }
  if (n == 0) { lastErr = "no headlines"; return false; }
  memcpy(NEWS, tmp, sizeof(Story) * n);
  newsN = n;
  Serial.printf("news: %d headlines, top='%s' (%s)\n", newsN, NEWS[0].title, NEWS[0].source);
  return true;
}

// Yahoo chart API, keyless. range=1d so chartPreviousClose is yesterday's close.
bool fetchQuotes() {
  JsonDocument filter;
  JsonObject m = filter["chart"]["result"][0].to<JsonObject>()["meta"].to<JsonObject>();
  m["regularMarketPrice"] = true; m["chartPreviousClose"] = true;
  int ok = 0;
  for (int i = 0; i < NTICK; i++) {
    String url = String("https://query1.finance.yahoo.com/v8/finance/chart/") + TICKERS[i].sym +
                 "?interval=1d&range=1d";
    size_t len;
    if (!httpGetRaw(url, len)) continue;
    JsonDocument doc;
    if (deserializeJson(doc, bodyBuf, len, DeserializationOption::Filter(filter))) { lastErr = "quote JSON"; continue; }
    JsonObject meta = doc["chart"]["result"][0]["meta"];
    float px = meta["regularMarketPrice"] | NAN;
    float pc = meta["chartPreviousClose"] | NAN;
    if (isnan(px) || px <= 0) continue;
    TICKERS[i].price = px;                                       // keep last good on failure
    if (!isnan(pc) && pc > 0) TICKERS[i].prev = pc;
    Serial.printf("  %-8s %10.2f  prev %10.2f\n", TICKERS[i].label, px, pc);
    ok++;
    delay(120);
  }
  Serial.printf("quotes: %d/%d ok\n", ok, NTICK);
  return ok == NTICK;
}

// ---------------- drawing ----------------
const int M = 16;
static int16_t textW(const String& s) { int16_t x, y; uint16_t w, h; display.getTextBounds(s, 0, 0, &x, &y, &w, &h); return w; }
static void txt(int x, int y, const String& s, const GFXfont* f, uint16_t col = C_BLACK) {
  display.setFont(f); display.setTextSize(1);
  display.setTextColor(col); display.setCursor(x, y); display.print(s);
}
static void txtR(int xr, int y, const String& s, const GFXfont* f, uint16_t col = C_BLACK) {
  display.setFont(f); display.setTextSize(1); txt(xr - textW(s), y, s, f, col);
}
static int wrapText(const String& s, const GFXfont* f, int maxW, String out[], int maxLines) {
  display.setFont(f); display.setTextSize(1);
  int n = 0; String line; int i = 0, len = s.length();
  while (i <= len && n < maxLines) {
    int j = i; while (j < len && s[j] != ' ') j++;
    String word = s.substring(i, j);
    String cand = line.length() ? line + " " + word : word;
    if (textW(cand) > maxW && line.length()) {
      if (n == maxLines - 1) { line += "..."; break; }         // last allowed line: ellipsis
      out[n++] = line; line = word;
    } else line = cand;
    if (j >= len) break;
    i = j + 1;
  }
  if (n < maxLines && line.length()) {
    while (textW(line) > maxW && line.length() > 4) line = line.substring(0, line.length() - 4) + "...";
    out[n++] = line;
  }
  return n;
}

const int HEAD_H = 46;
const int Q_COLS = 4, Q_ROW_H = 58;

void drawScreen() {
  time_t now; time(&now);
  display.fillScreen(C_WHITE);
  display.setTextWrap(false);

  // header bar
  display.fillRect(0, 0, SCR_W, HEAD_H, C_BLACK);
  txt(M, 32, SCREEN_TITLE, &FreeSansBold18pt7b, C_WHITE);
  txtR(SCR_W - M, 31, fmtTime(now, "%a %b %e  %l:%M %p"), &FreeSans12pt7b, C_WHITE);

  int qRows = (NTICK + Q_COLS - 1) / Q_COLS;
  int qTop = SCR_H - qRows * Q_ROW_H - 6;
  bool stale = !(newsOk && quotesOk);
  int newsBottom = qTop - (stale ? 26 : 6);

  // headlines
  int y = HEAD_H + 32;
  if (newsN == 0) txt(M, y + 20, "Headlines unavailable", &FreeSansBold18pt7b, C_RED);
  for (int i = 0; i < newsN; i++) {
    String ln[MAX_LINES];
    int n = wrapText(NEWS[i].title, &FreeSansBold12pt7b, SCR_W - 2 * M - 22, ln, MAX_LINES);
    int blockBottom = y + (n - 1) * 26 + 22;
    if (blockBottom > newsBottom) break;
    bool fresh = NEWS[i].t && now - NEWS[i].t < FRESH_MINUTES * 60;
    display.fillRect(M, y - 13, 10, 10, fresh ? C_RED : C_BLACK);
    for (int k = 0; k < n; k++) txt(M + 22, y + k * 26, ln[k], &FreeSansBold12pt7b);
    String meta = String(NEWS[i].source);
    String ago = agoStr(NEWS[i].t, now);
    if (ago.length()) meta += (meta.length() ? "   " : "") + ago;
    txt(M + 22, y + (n - 1) * 26 + 20, meta, &FreeSans9pt7b);
    y = blockBottom + 30;
  }

  if (stale) {
    String s = "Data may be old - last full update " + fmtTime(lastGood) + "  (" + lastErr + ")";
    txt(M, qTop - 10, s, &FreeSans9pt7b, C_RED);
  }

  // quotes grid
  display.fillRect(0, qTop, SCR_W, 2, C_BLACK);
  int colW = SCR_W / Q_COLS;
  for (int i = 0; i < NTICK; i++) {
    int cx = (i % Q_COLS) * colW, ry = qTop + 2 + (i / Q_COLS) * Q_ROW_H;
    if (i % Q_COLS) display.drawFastVLine(cx, ry + 6, Q_ROW_H - 10, C_BLACK);
    int x0 = cx + 12, xr = cx + colW - 12;
    const Ticker& t = TICKERS[i];
    txt(x0, ry + 20, t.label, &FreeSansBold9pt7b);
    if (!isnan(t.price) && !isnan(t.prev)) {
      float pct = (t.price - t.prev) / t.prev * 100.0f;
      bool down = pct < 0;
      uint16_t col = down ? C_RED : C_BLACK;
      char b[16]; snprintf(b, sizeof b, "%+.2f%%", pct);
      int tx = xr - 10;                                        // triangle on the right
      if (down) display.fillTriangle(tx - 6, ry + 9, tx + 6, ry + 9, tx, ry + 20, col);
      else      display.fillTriangle(tx - 6, ry + 20, tx + 6, ry + 20, tx, ry + 9, col);
      txtR(tx - 10, ry + 20, b, &FreeSansBold9pt7b, col);
    }
    txt(x0, ry + 50, fmtPrice(t.price), &FreeSansBold18pt7b);
  }
}

// Draw band by band, stream to the panel, ~23 s full refresh, then sleep the panel.
void bbepSendCMDSequence(BBEPDISP *pBBEP, const uint8_t *pSeq);   // bb_epaper internal
static void pushScreen(void (*draw)()) {
  if (panelAsleep) { bbep.wake(); panelAsleep = false; }
  bbepSendCMDSequence(&bbep._bbep, bbep._bbep.pInitFull);
  bbep.writeCmd(0x10);
  for (bandX0 = 0; bandX0 < SCR_W; bandX0 += BAND_W) {
    draw();
    for (int i = 0; i < BAND_W; i++) bbep.writeData(band + i * ROW_B, ROW_B);
  }
  bandX0 = 0;
  bbep.refresh(REFRESH_FULL, true);
  bbep.sleep(DEEP_SLEEP); panelAsleep = true;
}
static String rmBig, rmSmall;
static void drawRenderMessage() {
  display.fillScreen(C_WHITE);
  display.setTextWrap(false);
  display.fillRect(0, 0, SCR_W, HEAD_H, C_BLACK);
  txt(M, 32, SCREEN_TITLE, &FreeSansBold18pt7b, C_WHITE);
  txt(M, 220, rmBig, &FreeSansBold18pt7b, C_RED);
  txt(M, 260, rmSmall, &FreeSans12pt7b);
}
static void renderMessage(const String& big, const String& small) {
  rmBig = big; rmSmall = small;
  pushScreen(drawRenderMessage);
}

// ---------------- Wi-Fi ----------------
bool connectWiFi(uint32_t overall_ms) {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setHostname(HOSTNAME);
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
const uint32_t CYCLE_S = REFRESH_MINUTES * 60;
static time_t nextDue = 0;
static uint32_t lastCycleMs = 0;
static bool wifiMsgShown = false;

static void cycle() {
  lastErr = "";
  newsOk = false;
  for (int a = 0; a < 3 && !newsOk; a++) { newsOk = fetchNews(); if (!newsOk) delay(1500); }
  quotesOk = fetchQuotes();
  if (newsOk && quotesOk) time(&lastGood);
  uint32_t t0 = millis();
  pushScreen(drawScreen);
  Serial.printf("render done in %lu ms (heap %u)\n", (unsigned long)(millis() - t0), (unsigned)ESP.getFreeHeap());
  lastCycleMs = millis();
  time_t now = time(nullptr);
  nextDue = (now / CYCLE_S) * CYCLE_S + CYCLE_S;               // clock-aligned (:00, :10 ...)
  Serial.printf("next refresh at %s\n", fmtTime(nextDue, "%l:%M:%S %p").c_str());
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nNewsTicker starting (E2741QS0B3 BWRY, landscape 800x480)");
  band = (uint8_t*)malloc(BAND_W * ROW_B);
  bodyBuf = (char*)(psramFound() ? ps_malloc(BODY_CAP) : malloc(BODY_CAP));
  if (!band || !bodyBuf) { Serial.println("buffer alloc FAILED"); while (true) delay(1000); }
#ifdef PWR_EN_PIN
  pinMode(PWR_EN_PIN, OUTPUT); digitalWrite(PWR_EN_PIN, HIGH); delay(50);
#endif
  bbep.initIO(DC_PIN, RESET_PIN, BUSY_PIN, CS_PIN, MOSI_PIN, SCK_PIN);
  bbep.setRotation(ROTATION);

  bool wifi = connectWiFi(60000);
  configTzTime(TIME_ZONE, "pool.ntp.org", "time.nist.gov");
  if (!wifi) { renderMessage("No Wi-Fi", String("Could not join ") + WIFI_SSID + " - retrying"); wifiMsgShown = true; return; }
  uint32_t t0 = millis();
  while (time(nullptr) < 1000000000 && millis() - t0 < 8000) delay(100);
  Serial.printf("NTP %s after %lu ms\n", time(nullptr) < 1000000000 ? "NOT synced" : "synced", (unsigned long)(millis() - t0));
  cycle();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    if (connectWiFi(60000)) { wifiMsgShown = false; cycle(); }
    else if (!wifiMsgShown) { renderMessage("No Wi-Fi", String("Could not join ") + WIFI_SSID + " - retrying"); wifiMsgShown = true; }
    return;
  }
  time_t now = time(nullptr);
  bool due = now > 1000000000 ? now >= nextDue : millis() - lastCycleMs >= CYCLE_S * 1000UL;
  if (due) cycle();
  delay(1000);
}
