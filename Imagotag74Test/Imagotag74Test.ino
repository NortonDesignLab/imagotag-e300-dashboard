// Imagotag E300 7.4" BWRY (Pervasive E2741QS0B3, Spectra 4) on Waveshare Rev3 driver board
#include <bb_epaper.h>

#define DC_PIN    27
#define RESET_PIN 26
#define BUSY_PIN  25
#define CS_PIN    15
#define SCK_PIN   13
#define MOSI_PIN  14

BBEPAPER bbep(EP74YR_480x800);

void setup()
{
  Serial.begin(115200);
  bbep.initIO(DC_PIN, RESET_PIN, BUSY_PIN, CS_PIN, MOSI_PIN, SCK_PIN);
  bbep.allocBuffer();
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
  bbep.println("driven by ESP32 Rev3");
  bbep.writePlane();
  Serial.println("refreshing (about 23 s)...");
  bbep.refresh(REFRESH_FULL, true);
  Serial.println("done");
  bbep.sleep(DEEP_SLEEP);
}

void loop() {}
