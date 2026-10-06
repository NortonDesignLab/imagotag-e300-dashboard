// GxEPD2 demo for Waveshare e-Paper ESP32 Driver Board (Rev3, ESP32-WROOM-32E)
// Panel: HINK-E075A07-A0 = GDEH075Z90, 7.5" 880x528, black/white/RED, SSD1677 (BUSY active-high)
// Class: GxEPD2_750c_Z90 in 3-color (GxEPD2_3C) mode, paged at HEIGHT/4 to fit ESP32 RAM.
//
// Driver-board pin mapping (Waveshare fixed):
//   BUSY=25  RST=26  DC=27  CS=15  CLK=13  DIN(MOSI)=14
// SPI remap must come AFTER display.init(), which calls SPI.begin() on the default pins.

#include <GxEPD2_3C.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include "esp_mac.h"

GxEPD2_3C<GxEPD2_750c_Z90, GxEPD2_750c_Z90::HEIGHT / 4> display(
    GxEPD2_750c_Z90(/*CS=*/ 15, /*DC=*/ 27, /*RST=*/ 26, /*BUSY=*/ 25));

void drawScreen()
{
  display.setRotation(0);           // landscape 880x528
  display.setFullWindow();
  display.firstPage();
  do
  {
    display.fillScreen(GxEPD_WHITE);
    const int w = display.width(), h = display.height();

    // Border: black outer, red inner
    display.drawRect(6, 6, w - 12, h - 12, GxEPD_BLACK);
    display.drawRect(10, 10, w - 20, h - 20, GxEPD_RED);

    // Title
    display.setTextColor(GxEPD_BLACK);
    display.setFont(&FreeSansBold24pt7b);
    display.setCursor(48, 92);
    display.print("Hello from SARA");
    display.fillRect(48, 112, w - 96, 4, GxEPD_RED);

    // Body
    display.setFont(&FreeSans12pt7b);
    int y = 170; const int lh = 40;
    display.setCursor(48, y);          display.print("Board : ESP32-WROOM-32E  (Waveshare e-Paper ESP32 Driver Board Rev3)");
    y += lh; display.setCursor(48, y); display.print("Panel : HINK-E075A07-A0 = GDEH075Z90   7.5\"  880 x 528   B/W/Red");
    y += lh; display.setCursor(48, y); display.print("Driver: GxEPD2_750c_Z90  (SSD1677, BUSY active-high)");

    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char macbuf[40];
    snprintf(macbuf, sizeof(macbuf), "MAC   : %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    y += lh; display.setCursor(48, y); display.print(macbuf);

    // Shape row: alternating black / red to prove both planes
    int gx = 48, gy = 380, gs = 70;
    display.fillRect(gx, gy, gs, gs, GxEPD_BLACK);
    display.fillRect(gx + (gs + 20), gy, gs, gs, GxEPD_RED);
    display.fillCircle(gx + 2 * (gs + 20) + gs / 2, gy + gs / 2, gs / 2, GxEPD_BLACK);
    display.fillCircle(gx + 3 * (gs + 20) + gs / 2, gy + gs / 2, gs / 2, GxEPD_RED);
    display.drawRect(gx + 4 * (gs + 20), gy, gs, gs, GxEPD_BLACK);
    display.drawCircle(gx + 5 * (gs + 20) + gs / 2, gy + gs / 2, gs / 2, GxEPD_RED);

    // Footer
    display.setFont(&FreeSans9pt7b);
    display.setTextColor(GxEPD_RED);
    display.setCursor(48, h - 40);
    display.print("GxEPD2 demo flashed 2026-09-21  -  original Waveshare loader backed up");

    // Corner registration ticks (confirm edges of the 880x528 area)
    display.fillRect(6, 6, 30, 6, GxEPD_BLACK);      display.fillRect(6, 6, 6, 30, GxEPD_BLACK);
    display.fillRect(w - 36, h - 12, 30, 6, GxEPD_BLACK); display.fillRect(w - 12, h - 36, 6, 30, GxEPD_BLACK);
  }
  while (display.nextPage());
}

void setup()
{
  Serial.begin(115200);
  delay(200);
  Serial.println("\nGxEPD2 3-color demo starting (880x528, GxEPD2_750c_Z90)...");

  display.init(115200, true, 2, false);

  // Waveshare ESP32 Driver Board: remap SPI AFTER init() -> SCK=13, MISO=12, MOSI=14, SS=15
  SPI.end();
  SPI.begin(13, 12, 14, 15);

  drawScreen();

  Serial.println("Refresh complete. Entering hibernate.");
  display.hibernate();
}

void loop() {}
