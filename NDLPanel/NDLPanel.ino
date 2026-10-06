// Norton Design Lab, LLC message panel: Imagotag E300 7.4" BWRY (E2741QS0B3) on Waveshare Rev3 driver board.
// Default screen = NDL lockup. POST /message shows text beside the logo ("banner") or full screen.
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <bb_epaper.h>
#include <gfxfont.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include "secrets.h"
#include "ndl_message.h"

#define DC_PIN    27
#define RESET_PIN 26
#define BUSY_PIN  25
#define CS_PIN    15
#define SCK_PIN   13
#define MOSI_PIN  14
#define ROTATION  90          // 270 flips the picture 180 degrees
#define SW 800
#define SH 480
#define HOSTNAME "ndl-panel"

BBEPAPER bbep(EP74YR_480x800);
WebServer server(80);

static const GFXfont* FONTS[] = { &FreeSansBold24pt7b, &FreeSansBold18pt7b, &FreeSansBold12pt7b, &FreeSansBold9pt7b };
static const int NFONTS = 4;

struct Line { int start, len; };

// ---- state ----
static String   msgText;
static bool     msgFull = false;
static uint32_t msgTtlMs = 0, msgSetMs = 0;     // ttl 0 = until cleared
static bool     needRender = true, asleep = false;
static uint32_t lastRenderMs = 0;

// ---- text engine on GFX fonts (drawPixel based, with measuring + wrap + *red* markup) ----
static const GFXglyph* glyphOf(const GFXfont* f, char c) {
  if (c < f->first || c > f->last) c = '?';
  return &f->glyph[c - f->first];
}
static int textW(const GFXfont* f, const char* s, int n) {
  int w = 0;
  for (int i = 0; i < n; i++) if (s[i] != '*' && s[i] != '~') w += glyphOf(f, s[i])->xAdvance;
  return w;
}
static void drawGlyph1(const GFXfont* f, char c, int x, int baseY, int color) {
  const GFXglyph* g = glyphOf(f, c);
  const uint8_t* bm = f->bitmap + g->bitmapOffset;
  uint8_t bits = 0, bit = 0;
  for (int yy = 0; yy < g->height; yy++)
    for (int xx = 0; xx < g->width; xx++) {
      if (!(bit++ & 7)) bits = *bm++;
      if (bits & 0x80) bbep.drawPixel(x + g->xOffset + xx, baseY + g->yOffset + yy, color);
      bits <<= 1;
    }
}
static void drawGlyph(const GFXfont* f, char c, int x, int baseY, int color) {
  if (color == BBEP_YELLOW)                       // yellow on white is faint: ring it in black first
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++)
      if (dx || dy) drawGlyph1(f, c, x + dx, baseY + dy, BBEP_BLACK);
  drawGlyph1(f, c, x, baseY, color);
}
static const int MAXLINES = 24;
static int wrap(const GFXfont* f, const String& t, int maxW, Line* L) {   // returns line count, or -1 if too many
  int n = 0, i = 0, len = t.length();
  const char* s = t.c_str();
  while (i <= len) {
    int ls = i, lastBreak = -1, j = i;
    for (; j < len && s[j] != '\n'; j++) {
      if (s[j] == ' ') lastBreak = j;
      if (textW(f, s + ls, j - ls + 1) > maxW) {                 // overflowed at j
        int e = (lastBreak > ls) ? lastBreak : j;                // break at space, else mid-word
        if (e == j && j == ls) e = j + 1;                        // always make progress
        if (n >= MAXLINES) return -1;
        L[n++] = { ls, e - ls };
        i = (lastBreak > ls) ? lastBreak + 1 : e; goto next;
      }
    }
    if (n >= MAXLINES) return -1;
    L[n++] = { ls, j - ls };
    if (j >= len) break;
    i = j + 1;
    next:;
  }
  return n;
}
// draws t inside the box using the biggest font that fits; centered=true centers lines and block
static void drawMessage(const String& t, int bx, int by, int bw, int bh, bool centered) {
  Line L[MAXLINES];
  int fi = NFONTS - 1, n = 0;
  for (int k = 0; k < NFONTS; k++) {
    int c = wrap(FONTS[k], t, bw, L);
    if (c > 0 && c * FONTS[k]->yAdvance <= bh) { fi = k; n = c; break; }
    if (k == NFONTS - 1) n = c > 0 ? c : MAXLINES;
  }
  const GFXfont* f = FONTS[fi];
  n = wrap(f, t, bw, L); if (n < 0) n = MAXLINES;
  int maxLines = bh / f->yAdvance; if (n > maxLines) n = maxLines;       // clip rather than overflow
  int blockH = n * f->yAdvance;
  int y = by + (centered ? (bh - blockH) / 2 : 0) + (-(int)f->glyph['H' - f->first].yOffset);
  int col = BBEP_BLACK;
  const char* s = t.c_str();
  for (int li = 0; li < n; li++) {
    int x = bx;
    if (centered) x += (bw - textW(f, s + L[li].start, L[li].len)) / 2;
    for (int k = 0; k < L[li].len; k++) {
      char c = s[L[li].start + k];
      if (c == '*') { col = (col == BBEP_RED) ? BBEP_BLACK : BBEP_RED; continue; }
      if (c == '~') { col = (col == BBEP_YELLOW) ? BBEP_BLACK : BBEP_YELLOW; continue; }
      drawGlyph(f, c, x, y, col);
      x += glyphOf(f, c)->xAdvance;
    }
    y += f->yAdvance;
  }
}

// ---- screens ----
static void blit(const uint8_t* img) {
  for (int y = 0; y < SH; y++)
    for (int x = 0; x < SW; x++) {
      int i = y * SW + x;
      bbep.drawPixel(x, y, (pgm_read_byte(&img[i >> 2]) >> (6 - 2 * (i & 3))) & 3);
    }
}
static bool msgLive() {
  if (!msgText.length()) return false;
  if (msgTtlMs && millis() - msgSetMs >= msgTtlMs) return false;
  return true;
}
static void render() {
  if (asleep) { bbep.wake(); asleep = false; }
  if (!msgLive()) {
    msgText = "";
    blit(NDL_MSG);
  } else if (msgFull) {
    bbep.fillScreen(BBEP_WHITE);
    bbep.fillRect(0, SH - 14, SW, 14, BBEP_YELLOW);
    bbep.fillRect(30, 28, SW - 60, 5, BBEP_RED);
    drawMessage(msgText, 40, 48, SW - 80, SH - 48 - 52, true);
    // footer
    const char* ft = "Norton Design Lab, LLC";
    const GFXfont* f = &FreeSansBold9pt7b;
    int x = (SW - textW(f, ft, strlen(ft))) / 2, yb = SH - 28;
    for (const char* p = ft; *p; p++) { drawGlyph(f, *p, x, yb, BBEP_BLACK); x += glyphOf(f, *p)->xAdvance; }
  } else {
    blit(NDL_BASE);
    drawMessage(msgText, NDL_COL_X, NDL_COL_Y, NDL_COL_W, SH - 14 - 16 - NDL_COL_Y, false);
  }
  bbep.writePlane();
  Serial.println("refreshing (about 23 s)...");
  bbep.refresh(REFRESH_FULL, true);
  bbep.sleep(DEEP_SLEEP); asleep = true;
  lastRenderMs = millis();
  Serial.println("done");
}

// ---- web ----
static String esc(const String& s) {
  String o; for (char c : s) { if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else if (c == '&') o += "&amp;"; else if (c == '"') o += "&quot;"; else o += c; } return o;
}
static String ascii(const String& in) {   // GFX fonts are ASCII-only: transliterate common UTF-8 punctuation
  String o; const char* s = in.c_str();
  for (size_t i = 0; s[i]; ) {
    uint8_t c = s[i];
    if (c < 0x80) { if (c >= 0x20 || c == '\n') o += (char)c; i++; continue; }
    if (c == 0xE2 && s[i+1] && s[i+2]) {
      uint8_t b = s[i+2];
      if (b == 0x98 || b == 0x99) o += '\'';
      else if (b == 0x9C || b == 0x9D) o += '"';
      else if (b == 0x93 || b == 0x94) o += '-';
      else if (b == 0xA6) o += "...";
      i += 3; continue;
    }
    i += (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : 2;       // skip other multibyte sequences
  }
  return o;
}
static String statusLine() {
  if (!msgLive()) return "Showing the Norton Design Lab default screen.";
  String s = String("Showing (") + (msgFull ? "full screen" : "beside logo") + ", ";
  if (!msgTtlMs) s += "until cleared";
  else { uint32_t left = (msgTtlMs - (millis() - msgSetMs)) / 60000UL + 1; s += String(left) + " min left"; }
  return s + "): " + msgText;
}
static const char FORM_HTML[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>Norton Design Lab panel</title><style>body{font-family:Montserrat,sans-serif;max-width:520px;margin:2em auto;padding:0 1em;color:#0e2243}
h2 span{color:#a02828}textarea,input,select,button{width:100%;font-size:1.05em;margin:.35em 0;box-sizing:border-box;padding:.5em}
button{background:#0e2243;color:#fff;border:0;border-radius:6px}.c button{background:#666}p.s{background:#f3f3f3;padding:.7em;border-radius:6px}</style></head><body>
<h2>Norton Design Lab<span>, LLC</span> panel</h2><p class=s>%STATUS%</p>
<form method=post action=/message><textarea name=text rows=4 maxlength=400 placeholder="Message (Enter = new line; *red* and ~yellow~ words)"></textarea>
<select name=style><option value=banner>Beside the logo</option><option value=full>Full screen</option></select>
<small>Tip: wrap words in *asterisks* for red, ~tildes~ for yellow.</small><label>Minutes to show (0 = until cleared)</label><input name=ttl type=number value=60 min=0>
<button>Show on panel (takes ~25 s)</button></form>
<form class=c method=post action=/clear><button>Clear message (back to the logo screen)</button></form></body></html>)HTML";

static void back() {
  const char* h[] = {"Accept"}; (void)h;
  if (server.header("Accept").indexOf("text/html") >= 0) { server.sendHeader("Location", "/"); server.send(303); }
  else server.send(200, "text/plain", statusLine() + "\n");
}
static void startWeb() {
  const char* hdrs[] = {"Accept"}; server.collectHeaders(hdrs, 1);
  server.on("/", HTTP_GET, [](){ String h = FPSTR(FORM_HTML); h.replace("%STATUS%", esc(statusLine())); server.send(200, "text/html", h); });
  server.on("/message", [](){
    String t = ascii(server.arg("text")); t.trim();
    if (!t.length()) { server.send(400, "text/plain", "missing text\n"); return; }
    if (t.length() > 400) t = t.substring(0, 400);
    msgText = t; msgFull = server.arg("style") == "full";
    long ttl = server.hasArg("ttl") ? server.arg("ttl").toInt() : 60;
    msgTtlMs = ttl > 0 ? (uint32_t)ttl * 60000UL : 0; msgSetMs = millis(); needRender = true;
    Serial.printf("message set (%s, ttl %ld min): %s\n", msgFull ? "full" : "banner", ttl, t.c_str());
    back();
  });
  server.on("/clear", [](){ if (msgText.length()) { msgText = ""; needRender = true; Serial.println("message cleared"); } back(); });
  server.on("/status", HTTP_GET, [](){ server.send(200, "text/plain", statusLine() + "\n"); });
  server.begin();
  if (MDNS.begin(HOSTNAME)) { MDNS.addService("http", "tcp", 80); Serial.println("mDNS: http://" HOSTNAME ".local"); }
}
static bool connectWiFi(uint32_t overall_ms) {         // retry loop: WPA2/WPA3-mixed APs need it on WROOM-32
  WiFi.persistent(false); WiFi.mode(WIFI_STA); WiFi.setSleep(false); WiFi.setHostname(HOSTNAME);
  uint32_t start = millis(); int attempt = 0;
  while (millis() - start < overall_ms) {
    attempt++; WiFi.disconnect(true, true); delay(250); WiFi.begin(WIFI_SSID, WIFI_PASS);
    uint32_t a0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - a0 < 8000) delay(150);
    if (WiFi.status() == WL_CONNECTED) { Serial.printf("WiFi connected as %s (RSSI %d, try %d)\n", WiFi.localIP().toString().c_str(), WiFi.RSSI(), attempt); return true; }
    Serial.printf("WiFi try %d failed (status %d)\n", attempt, WiFi.status());
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  bbep.initIO(DC_PIN, RESET_PIN, BUSY_PIN, CS_PIN, MOSI_PIN, SCK_PIN);
  bbep.allocBuffer();
  bbep.setRotation(ROTATION);
  connectWiFi(60000);
  startWeb();
  render();                         // first paint = default NDL screen
  needRender = false;
}
void loop() {
  if (WiFi.status() != WL_CONNECTED) { connectWiFi(60000); return; }
  server.handleClient();
  if (msgText.length() && !msgLive()) { msgText = ""; needRender = true; Serial.println("message expired"); }
  if (needRender) { needRender = false; render(); }
  delay(5);
}
