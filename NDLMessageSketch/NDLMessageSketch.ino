// Norton Design Lab, LLC message, landscape, on Imagotag E300 7.4 BWRY (E2741QS0B3)
#include <bb_epaper.h>
#include "ndl_message.h"

#define DC_PIN    27
#define RESET_PIN 26
#define BUSY_PIN  25
#define CS_PIN    15
#define SCK_PIN   13
#define MOSI_PIN  14
#define ROTATION  90   // 270 flips the picture 180 degrees

BBEPAPER bbep(EP74YR_480x800);

void setup()
{
  Serial.begin(115200);
  bbep.initIO(DC_PIN, RESET_PIN, BUSY_PIN, CS_PIN, MOSI_PIN, SCK_PIN);
  bbep.allocBuffer();
  bbep.setRotation(ROTATION);
  const int w = 800, h = 480;
  Serial.printf("logical %d x %d\n", bbep.width(), bbep.height());
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      int i = y * w + x;
      uint8_t c = (pgm_read_byte(&NDL_MSG[i >> 2]) >> (6 - 2 * (i & 3))) & 3;
      bbep.drawPixel(x, y, c);   // 0 black 1 white 2 yellow 3 red = BBEP_* values
    }
  }
  bbep.writePlane();
  Serial.println("refreshing (about 23 s)...");
  bbep.refresh(REFRESH_FULL, true);
  Serial.println("done");
  bbep.sleep(DEEP_SLEEP);
}

void loop() {}
