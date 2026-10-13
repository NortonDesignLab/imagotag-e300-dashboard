// Imagotag E300 7.4" BWRY (Pervasive E2741QS0B3, Spectra 4) on the
// Seeed XIAO ePaper Display Board EE04 + XIAO ESP32-S3 Plus.
// Pins = bb_epaper's "Seeed Xiao ePaper Display Board (ESP32-S3)" preset.
#include <bb_epaper.h>

#define DC_PIN    10
#define RESET_PIN 38
#define BUSY_PIN  4
#define CS_PIN    44
#define SCK_PIN   7
#define MOSI_PIN  9

#define KEY1_PIN 2   // D1, active low
#define KEY2_PIN 3   // D2
#define KEY3_PIN 5   // D4
#define VBAT_PIN 1   // A0, divider /2
#define VBAT_EN  6   // A5, HIGH to enable the divider

BBEPAPER bbep(EP74YR_480x800);

void setup()
{
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 4000) delay(10);  // USB-CDC: wait for host
  pinMode(KEY1_PIN, INPUT_PULLUP); pinMode(KEY2_PIN, INPUT_PULLUP); pinMode(KEY3_PIN, INPUT_PULLUP);
  pinMode(VBAT_EN, OUTPUT); digitalWrite(VBAT_EN, HIGH); delay(10);
  Serial.printf("EE04 test: heap %u, psram %u, vbat %.2f V, keys %d%d%d\n",
                ESP.getFreeHeap(), ESP.getPsramSize(),
                analogReadMilliVolts(VBAT_PIN) * 2.0f / 1000.0f,
                digitalRead(KEY1_PIN), digitalRead(KEY2_PIN), digitalRead(KEY3_PIN));

  bbep.initIO(DC_PIN, RESET_PIN, BUSY_PIN, CS_PIN, MOSI_PIN, SCK_PIN);
  bbep.setRotation(90);                 // landscape 800x480
  bbep.allocBuffer();                   // 96 KB, fine with 8 MB PSRAM / 320 KB heap on the S3
  int w = bbep.width(), h = bbep.height();
  Serial.printf("panel %d x %d\n", w, h);
  bbep.fillScreen(BBEP_WHITE);
  int bar = h / 8;
  bbep.fillRect(0, 0,       w, bar, BBEP_BLACK);
  bbep.fillRect(0, bar * 2, w, bar, BBEP_RED);
  bbep.fillRect(0, bar * 4, w, bar, BBEP_YELLOW);
  bbep.setFont(FONT_12x16);
  bbep.setTextColor(BBEP_BLACK);
  bbep.setCursor(10, bar * 6);
  bbep.println("Imagotag E300 7.4 BWRY");
  bbep.setTextColor(BBEP_RED);
  bbep.println("Seeed XIAO EE04 + ESP32-S3 Plus");
  bbep.writePlane();
  Serial.println("refreshing (about 23 s)...");
  t0 = millis();
  bbep.refresh(REFRESH_FULL, true);
  Serial.printf("done in %lu ms\n", millis() - t0);
  bbep.sleep(DEEP_SLEEP);
}

void loop()
{
  static int last = -1;
  int k = (!digitalRead(KEY1_PIN)) | (!digitalRead(KEY2_PIN)) << 1 | (!digitalRead(KEY3_PIN)) << 2;
  if (k != last) { Serial.printf("keys: %d%d%d\n", k & 1, (k >> 1) & 1, (k >> 2) & 1); last = k; }
  delay(50);
}
