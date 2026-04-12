#include "Arduino.h"
#include "Display_ST7701.h"

void fillScreen(uint16_t color) {
  uint16_t *buf = (uint16_t*)heap_caps_malloc(480 * 10 * 2, MALLOC_CAP_SPIRAM);
  if (!buf) return;
  for (int i = 0; i < 480 * 10; i++) buf[i] = color;
  for (int y = 0; y < 480; y += 10) {
    LCD_addWindow(0, y, 479, y + 9, (uint8_t*)buf);
  }
  free(buf);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  LCD_Init();
}

void loop() {
  delay(1000);
  fillScreen(0xF800);  // red
  delay(1000);
  fillScreen(0x07E0);  // green
  delay(1000);
  fillScreen(0x001F);  // blue
}