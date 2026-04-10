// ═══════════════════════════════════════════════════════════════
//  ui_cyberpi.cpp — UI implementation for CyberPi (128x128 ST7735)
//  Implements the ui.h interface.
//  Hardware: CyberPi ESP32, AW9523B expander, ST7735 LCD
// ═══════════════════════════════════════════════════════════════
#include "ui.h"
#include "app_state.h"
#include "bt.h"
#include "obd.h"
#include "pids.h"
#include <Preferences.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <math.h>

// ─────────────────────────────────────────────────────────────
// CyberPi Hardware
// ─────────────────────────────────────────────────────────────
#define LCD_CS    12
#define LCD_MOSI   2
#define LCD_SCLK   4
#define LCD_MISO  26

#define AW9523B_ADDR   0x58
#define AW_P1_OUT_REG  0x03
#define AW_P1_CFG_REG  0x05
#define AW_DC_BIT   (1 << 4)
#define AW_RST_BIT  (1 << 5)
#define AW_BL_BIT   (1 << 7)
#define AW_AMP_BIT  (1 << 3)

#define SCREEN_W  128
#define SCREEN_H  128

// ─────────────────────────────────────────────────────────────
// Colors RGB565
// ─────────────────────────────────────────────────────────────
#define TFT_BLACK   0x0000
#define TFT_WHITE   0xFFFF
#define TFT_YELLOW  0xFFE0
#define TFT_CYAN    0x07FF
#define TFT_RED     0xF800
#define TFT_GREEN   0x07E0
#define TFT_GRAY    0x8410
#define TFT_DGRAY   0x2104

// ─────────────────────────────────────────────────────────────
// Framebuffer — 128x128 x 2 bytes = 32KB
// Draw everything here, flush to screen in one burst
// ─────────────────────────────────────────────────────────────
static uint16_t framebuf[SCREEN_W * SCREEN_H];

// ─────────────────────────────────────────────────────────────
// AW9523B
// ─────────────────────────────────────────────────────────────
static uint8_t aw_p1_state = 0x00;

static void aw_write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(AW9523B_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

static void aw_p1_set(uint8_t bit, bool high) {
  if (high) aw_p1_state |=  bit;
  else      aw_p1_state &= ~bit;
  aw_write(AW_P1_OUT_REG, aw_p1_state);
}

// ─────────────────────────────────────────────────────────────
// RGB LEDs — AW9523B at addr 0x5B, PWM mode
// 5 LEDs × 3 bytes (R,G,B) = 15 bytes starting at REG_DIM00 (0x20)
// LED order on CyberPi: LED0=left … LED4=right
// ─────────────────────────────────────────────────────────────
#define RGB_ADDR     0x5B
#define RGB_REG_DIM  0x20   // first PWM dimming register

static void rgb_write_all(uint8_t r0, uint8_t g0, uint8_t b0,
                          uint8_t r1, uint8_t g1, uint8_t b1,
                          uint8_t r2, uint8_t g2, uint8_t b2,
                          uint8_t r3, uint8_t g3, uint8_t b3,
                          uint8_t r4, uint8_t g4, uint8_t b4) {
  Wire.beginTransmission(RGB_ADDR);
  Wire.write(RGB_REG_DIM);
  Wire.write(r0); Wire.write(g0); Wire.write(b0);
  Wire.write(r1); Wire.write(g1); Wire.write(b1);
  Wire.write(r2); Wire.write(g2); Wire.write(b2);
  Wire.write(r3); Wire.write(g3); Wire.write(b3);
  Wire.write(r4); Wire.write(g4); Wire.write(b4);
  Wire.endTransmission();
}

void ui_leds_off() {
  rgb_write_all(0,0,0, 0,0,0, 0,0,0, 0,0,0, 0,0,0);
}

// Forward declaration — defined later alongside drawDial
static uint16_t arcSegColor(float segPct, float warnPct);

// LED bar — pct 0.0-1.0, warnPct = warn threshold as fraction of max
// 5 LEDs fill left to right, color follows arc zone colors
void ui_leds(float pct, float warnPct) {
  const float thresholds[5] = { 0.1f, 0.3f, 0.5f, 0.7f, 0.9f };
  uint8_t buf[15] = {0};
  for (int i = 0; i < 5; i++) {
    if (pct >= thresholds[i]) {
      uint16_t col = arcSegColor(thresholds[i], warnPct);
      // Extract R5G6B5 → scale to 0-40 brightness
      uint8_t r = (uint8_t)(((col >> 11) & 0x1F) * 40 / 31);
      uint8_t g = (uint8_t)(((col >>  5) & 0x3F) * 40 / 63);
      uint8_t b = (uint8_t)(((col >>  0) & 0x1F) * 40 / 31);
      buf[i*3+0] = r;
      buf[i*3+1] = g;
      buf[i*3+2] = b;
    }
  }
  Wire.beginTransmission(RGB_ADDR);
  Wire.write(RGB_REG_DIM);
  for (int i = 0; i < 15; i++) Wire.write(buf[i]);
  Wire.endTransmission();
}

static void initRGBLeds() {
  // Matches the working RGB_Test sketch exactly
  Wire.beginTransmission(RGB_ADDR);
  Wire.write(0x11); Wire.write(0x10);  // REG_CTL: push-pull mode for port 0
  Wire.endTransmission();
  Wire.beginTransmission(RGB_ADDR);
  Wire.write(0x04); Wire.write(0x00);  // REG_P0_CFG: all P0 output
  Wire.endTransmission();
  Wire.beginTransmission(RGB_ADDR);
  Wire.write(0x05); Wire.write(0x00);  // REG_P1_CFG: all P1 output
  Wire.endTransmission();
  Wire.beginTransmission(RGB_ADDR);
  Wire.write(0x12); Wire.write(0x00);  // REG_P0_MODE: LED/PWM mode
  Wire.endTransmission();
  Wire.beginTransmission(RGB_ADDR);
  Wire.write(0x13); Wire.write(0x00);  // REG_P1_MODE: LED/PWM mode
  Wire.endTransmission();
  delay(20);
  ui_leds_off();
}


static SPISettings spiSettings(27000000, MSBFIRST, SPI_MODE0);

static void cs_low()  { digitalWrite(LCD_CS, LOW);  }
static void cs_high() { digitalWrite(LCD_CS, HIGH); }

static void writeCommand(uint8_t cmd) {
  aw_p1_set(AW_DC_BIT, false);
  SPI.beginTransaction(spiSettings);
  cs_low();
  SPI.transfer(cmd);
  cs_high();
  SPI.endTransaction();
}

static void writeData(uint8_t data) {
  aw_p1_set(AW_DC_BIT, true);
  SPI.beginTransaction(spiSettings);
  cs_low();
  SPI.transfer(data);
  cs_high();
  SPI.endTransaction();
}

// ─────────────────────────────────────────────────────────────
// Flush framebuffer to screen in one SPI burst
// ─────────────────────────────────────────────────────────────
// ST7735 128x128 black tab offset — display RAM starts at col 2, row 1
#define COL_OFFSET 3
#define ROW_OFFSET 2

static void flushFramebuf() {
  writeCommand(0x2A);
  writeData(0x00); writeData(COL_OFFSET);
  writeData(0x00); writeData(SCREEN_W - 1 + COL_OFFSET);
  writeCommand(0x2B);
  writeData(0x00); writeData(ROW_OFFSET);
  writeData(0x00); writeData(SCREEN_H - 1 + ROW_OFFSET);
  writeCommand(0x2C);

  // Blast all pixels — DC stays HIGH for entire transfer
  aw_p1_set(AW_DC_BIT, true);
  SPI.beginTransaction(spiSettings);
  cs_low();
  // Send as bytes — swap high/low bytes for correct endianness
  uint8_t* p = (uint8_t*)framebuf;
  for (int i = 0; i < SCREEN_W * SCREEN_H * 2; i++) {
    SPI.transfer(p[i]);
  }
  cs_high();
  SPI.endTransaction();
}

// ─────────────────────────────────────────────────────────────
// GFX subclass — draws into framebuffer, never touches SPI
// ─────────────────────────────────────────────────────────────
class CyberPiGFX : public Adafruit_GFX {
public:
  CyberPiGFX() : Adafruit_GFX(SCREEN_W, SCREEN_H) {}

  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) return;
    // Store big-endian RGB565 (swap bytes for hardware)
    uint16_t c = (color >> 8) | (color << 8);
    framebuf[y * SCREEN_W + x] = c;
  }

  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCREEN_W) w = SCREEN_W - x;
    if (y + h > SCREEN_H) h = SCREEN_H - y;
    if (w <= 0 || h <= 0) return;
    uint16_t c = (color >> 8) | (color << 8);
    for (int16_t j = y; j < y + h; j++) {
      uint16_t* row = &framebuf[j * SCREEN_W + x];
      for (int16_t i = 0; i < w; i++) row[i] = c;
    }
  }
};

static CyberPiGFX tft;

// ─────────────────────────────────────────────────────────────
// ST7735 init sequence
// ─────────────────────────────────────────────────────────────
static void st7735_init() {
  aw_p1_set(AW_RST_BIT, false); delay(20);
  aw_p1_set(AW_RST_BIT, true);  delay(150);

  writeCommand(0x01); delay(150);  // SWRESET
  writeCommand(0x11); delay(500);  // SLPOUT

  writeCommand(0xB1);
  writeData(0x01); writeData(0x2C); writeData(0x2D);
  writeCommand(0xB2);
  writeData(0x01); writeData(0x2C); writeData(0x2D);
  writeCommand(0xB3);
  writeData(0x01); writeData(0x2C); writeData(0x2D);
  writeData(0x01); writeData(0x2C); writeData(0x2D);

  writeCommand(0xB4); writeData(0x07);

  writeCommand(0xC0);
  writeData(0xA2); writeData(0x02); writeData(0x84);
  writeCommand(0xC1); writeData(0xC5);
  writeCommand(0xC2); writeData(0x0A); writeData(0x00);
  writeCommand(0xC3); writeData(0x8A); writeData(0x2A);
  writeCommand(0xC4); writeData(0x8A); writeData(0xEE);
  writeCommand(0xC5); writeData(0x0E);

  // MADCTL: MY=1 MV=1 BGR=1 → 90deg CW
  writeCommand(0x36); writeData(0xA8);

  writeCommand(0x3A); writeData(0x05);  // 16bit color

  writeCommand(0xE0);
  writeData(0x02); writeData(0x1C); writeData(0x07); writeData(0x12);
  writeData(0x37); writeData(0x32); writeData(0x29); writeData(0x2D);
  writeData(0x29); writeData(0x25); writeData(0x2B); writeData(0x39);
  writeData(0x00); writeData(0x01); writeData(0x03); writeData(0x10);
  writeCommand(0xE1);
  writeData(0x03); writeData(0x1D); writeData(0x07); writeData(0x06);
  writeData(0x2E); writeData(0x2C); writeData(0x29); writeData(0x2D);
  writeData(0x2E); writeData(0x2E); writeData(0x37); writeData(0x3F);
  writeData(0x00); writeData(0x00); writeData(0x02); writeData(0x10);

  writeCommand(0x13); delay(10);
  writeCommand(0x29); delay(100);

  Serial.println("ST7735 init done");
}

// ─────────────────────────────────────────────────────────────
// VextON — no-op
// ─────────────────────────────────────────────────────────────
void VextON() {}

// ─────────────────────────────────────────────────────────────
// initDisplay
// ─────────────────────────────────────────────────────────────
void ui_init() {
  Wire.begin(19, 18);
  delay(10);

  aw_write(AW_P1_CFG_REG, 0x00);
  delay(5);
  aw_p1_set(AW_AMP_BIT, false);  // audio amp OFF
  aw_p1_set(AW_BL_BIT,  false);  // backlight OFF during init — prevents white flash
  aw_p1_set(AW_DC_BIT,  true);
  aw_p1_set(AW_RST_BIT, false);  // hold reset LOW immediately
  delay(20);
  aw_p1_set(AW_RST_BIT, true);   // release reset
  delay(50);

  SPI.begin(LCD_SCLK, LCD_MISO, LCD_MOSI, LCD_CS);
  pinMode(LCD_CS, OUTPUT);
  cs_high();

  st7735_init();

  // Clear framebuffer, flush black frame, THEN turn on backlight
  memset(framebuf, 0, sizeof(framebuf));
  flushFramebuf();
  aw_p1_set(AW_BL_BIT, true);    // backlight ON — clean black screen visible first

  Serial.println("Display ready");
  initRGBLeds();
}

// ─────────────────────────────────────────────────────────────
// Drawing helpers — all draw to framebuffer
// ─────────────────────────────────────────────────────────────

static void drawTitle(const char* title) {
  tft.fillRect(0, 0, SCREEN_W, 14, TFT_DGRAY);
  tft.drawFastHLine(0, 14, SCREEN_W, TFT_GRAY);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(1);
  tft.setCursor(2, 3);
  tft.print(title);
}

static void drawCentered(const char* text, int y, int size, uint16_t color) {
  tft.setTextSize(size);
  tft.setTextColor(color, TFT_BLACK);
  int16_t x1, y1;
  uint16_t w, h;
  tft.getTextBounds(text, 0, y, &x1, &y1, &w, &h);
  tft.setCursor((SCREEN_W - w) / 2, y);
  tft.print(text);
}

static void drawProgressBar(int x, int y, int w, int h, int pct) {
  tft.drawRect(x, y, w, h, TFT_WHITE);
  int filled = (int)((w - 2) * pct / 100.0);
  if (filled > 0)
    tft.fillRect(x + 1, y + 1, filled, h - 2, TFT_CYAN);
  if (filled < w - 2)
    tft.fillRect(x + 1 + filled, y + 1, w - 2 - filled, h - 2, TFT_BLACK);
}

// ─────────────────────────────────────────────────────────────
// Screens — all draw to framebuffer then flush once at the end

// ─────────────────────────────────────────────────────────────
// Button icon helpers
// drawJoystick(x,y,color) — draws a small circle with dot (PS2 style)
// drawSquareBtn(x,y,color) — draws a small filled square
// drawTriangleBtn(x,y,color) — draws a small triangle
// drawHoldIndicator(x,y,color) — draws a hold/long-press arc
//
// Usage: icon + optional label next to it
// All icons are ~10px tall, designed for hint rows
// ─────────────────────────────────────────────────────────────

static void drawJoystickIcon(int cx, int cy, uint16_t color) {
  tft.drawCircle(cx, cy, 5, color);       // outer ring
  tft.fillCircle(cx, cy, 2, color);       // center dot
  // 4 direction nubs
  tft.drawPixel(cx, cy - 5, color);
  tft.drawPixel(cx, cy + 5, color);
  tft.drawPixel(cx - 5, cy, color);
  tft.drawPixel(cx + 5, cy, color);
}

static void drawSquareIcon(int cx, int cy, uint16_t color) {
  tft.drawRect(cx - 4, cy - 4, 9, 9, color);
  tft.drawRect(cx - 3, cy - 3, 7, 7, color);
}

static void drawTriangleIcon(int cx, int cy, uint16_t color) {
  // Upward triangle
  tft.drawLine(cx, cy - 5, cx - 5, cy + 4, color);
  tft.drawLine(cx - 5, cy + 4, cx + 5, cy + 4, color);
  tft.drawLine(cx + 5, cy + 4, cx, cy - 5, color);
}

static void drawHoldArc(int cx, int cy, uint16_t color) {
  // Small arc indicating hold/long-press
  tft.drawCircle(cx, cy, 4, color);
  tft.fillRect(cx - 1, cy - 4, 3, 4, TFT_BLACK);  // gap at top
  tft.drawFastVLine(cx, cy - 4, 2, color);          // arrow down
  tft.drawPixel(cx - 1, cy - 3, color);
  tft.drawPixel(cx + 1, cy - 3, color);
}

// Draw a hint row: icon + label
// x = left edge, y = vertical center
static void drawHint(int x, int y, const char* icon, const char* label,
                     uint16_t iconColor, uint16_t textColor) {
  int ix = x + 5;
  if (strcmp(icon, "joy") == 0)  drawJoystickIcon(ix, y, iconColor);
  if (strcmp(icon, "sq") == 0)   drawSquareIcon(ix, y, iconColor);
  if (strcmp(icon, "tri") == 0)  drawTriangleIcon(ix, y, iconColor);
  if (strcmp(icon, "hold") == 0) drawHoldArc(ix, y, iconColor);
  tft.setTextSize(1);
  tft.setTextColor(textColor, TFT_BLACK);
  tft.setCursor(x + 13, y - 3);
  tft.print(label);
}

// Draw a centered hint bar at the bottom of screen
// Up to 2 hints side by side
static void drawHintBar(const char* icon1, const char* label1,
                        const char* icon2, const char* label2) {
  tft.drawFastHLine(0, 108, SCREEN_W, TFT_DGRAY);
  tft.fillRect(0, 109, SCREEN_W, 19, TFT_BLACK);
  if (icon2 && strlen(icon2) > 0) {
    // Two hints — each gets half the screen (64px)
    // Icon=12px, text starts at +13, max 8 chars (48px) = 61px total — fits
    drawHint(2,  118, icon1, label1, TFT_CYAN, TFT_GRAY);
    drawHint(66, 118, icon2, label2, TFT_CYAN, TFT_GRAY);
  } else {
    int w = (int)strlen(label1) * 6 + 14;
    drawHint((SCREEN_W - w) / 2, 118, icon1, label1, TFT_CYAN, TFT_GRAY);
  }
}
// ─────────────────────────────────────────────────────────────


static unsigned long autoConnectFirstCall = 0;

void ui_resetAutoConnect() {
  autoConnectFirstCall = 0;
}

void ui_autoConnect() {
  static int           frame     = 0;
  static unsigned long lastFrame = 0;
  if (millis() - lastFrame > 150) { frame = (frame + 1) % 8; lastFrame = millis(); }
  const char* spinner[] = { "|", "/", "-", "\\", "|", "/", "-", "\\" };

  if (autoConnectFirstCall == 0) autoConnectFirstCall = millis();
  // Fill bar over 5s matching the timeout in the .ino
  int pct = (int)(100.0f * min((millis() - autoConnectFirstCall) / 5000.0f, 1.0f));

  tft.fillScreen(TFT_BLACK);

  // Spinner
  tft.setTextSize(2);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(58, 8);
  tft.print(spinner[frame]);

  tft.drawFastHLine(0, 28, SCREEN_W, TFT_DGRAY);

  drawCentered("Reconnecting to", 33, 1, TFT_GRAY);

  String name = getSavedDeviceName();
  if (name == "") name = "Saved device";
  if (name.length() <= 10) {
    drawCentered(name.c_str(), 50, 2, TFT_WHITE);
  } else {
    drawCentered(name.substring(0, 16).c_str(), 46, 1, TFT_WHITE);
    if (name.length() > 16)
      drawCentered(name.substring(16, 32).c_str(), 58, 1, TFT_WHITE);
  }

  // Progress bar — fills left to right over 5s
  int filled = (int)(118.0f * (pct / 100.0f));
  tft.drawRect(4, 80, 120, 8, TFT_DGRAY);
  if (filled > 0)   tft.fillRect(5, 81, filled, 6, TFT_CYAN);
  if (filled < 118) tft.fillRect(5 + filled, 81, 118 - filled, 6, TFT_BLACK);

  // Status hint at bottom
  tft.setTextSize(1);
  tft.setTextColor(TFT_GRAY, TFT_BLACK);
  tft.setCursor(2, 112);
  tft.print(pct < 20 ? "Waiting..." : pct < 80 ? "Connecting..." : "Almost...");

  flushFramebuf();
}

void ui_connecting() {
  static int           frame     = 0;
  static unsigned long lastFrame = 0;
  if (millis() - lastFrame > 200) { frame = (frame + 1) % 8; lastFrame = millis(); }
  const char* spinner[] = { "|", "/", "-", "\\", "|", "/", "-", "\\" };

  tft.fillScreen(TFT_BLACK);

  tft.setTextSize(2);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setCursor(58, 8);
  tft.print(spinner[frame]);

  tft.drawFastHLine(0, 28, SCREEN_W, TFT_DGRAY);

  drawCentered("Connecting", 36, 1, TFT_GRAY);

  String name = getSavedDeviceName();
  if (name == "") name = "OBD Adapter";
  // Use size 2 for short names, size 1 for longer ones
  if (name.length() <= 10) {
    drawCentered(name.c_str(), 52, 2, TFT_WHITE);
  } else {
    // Split to two lines at size 1
    drawCentered(name.substring(0, 16).c_str(), 48, 1, TFT_WHITE);
    if (name.length() > 16)
      drawCentered(name.substring(16, 32).c_str(), 60, 1, TFT_WHITE);
  }

  tft.drawFastHLine(0, 98, SCREEN_W, TFT_DGRAY);
  drawCentered("Please wait...", 104, 1, TFT_GRAY);

  flushFramebuf();
}

static void showWaitingData() {
  static int           frame     = 0;
  static unsigned long lastFrame = 0;
  if (millis() - lastFrame > 200) { frame = (frame + 1) % 8; lastFrame = millis(); }
  const char* spinner[] = { "|", "/", "-", "\\", "|", "/", "-", "\\" };

  tft.fillScreen(TFT_BLACK);

  tft.setTextSize(2);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setCursor(58, 8);
  tft.print(spinner[frame]);

  tft.drawFastHLine(0, 28, SCREEN_W, TFT_DGRAY);

  drawCentered("Connected!", 36, 1, TFT_GRAY);

  String name = getSavedDeviceName();
  if (name == "") name = "OBD Adapter";
  if (name.length() <= 10) {
    drawCentered(name.c_str(), 52, 2, TFT_WHITE);
  } else {
    drawCentered(name.substring(0, 16).c_str(), 48, 1, TFT_WHITE);
    if (name.length() > 16)
      drawCentered(name.substring(16, 32).c_str(), 60, 1, TFT_WHITE);
  }

  tft.drawFastHLine(0, 78, SCREEN_W, TFT_DGRAY);
  drawCentered("Getting data...", 84, 1, TFT_GRAY);

  flushFramebuf();
}

// ─────────────────────────────────────────────────────────────
// Virtual device list helpers
// ─────────────────────────────────────────────────────────────

// ── Virtual list layout ─────────────────────────────────────
// index 0        → SAVED device (if exists) — hold to forget
// index 1..N     → scanned BLE devices (deviceList[index-offset])
// ─────────────────────────────────────────────────────────────

static int virtualOffset() {
  return (getSavedDeviceName() != "") ? 1 : 0;
}

int getVirtualCount() {
  return getDeviceCount() + virtualOffset();
}

bool isSavedSlot(int index) {
  return (getSavedDeviceName() != "" && index == 0);
}

bool isForgetSlot(int index) {
  return isSavedSlot(index);
}

// Convert virtual index to real deviceList index
int virtualToReal(int index) {
  return index - virtualOffset();
}

static int scanLastSelected = -1;
static int scanLastCount    = -1;

void ui_resetScanDisplay() {
  scanLastSelected = -2;  // force redraw on next call
  scanLastCount    = -2;
}

void ui_menuConnect(int deviceCount, int selected) {
  int vCount = getVirtualCount();

  tft.fillScreen(TFT_BLACK);

  // Title bar
  tft.fillRect(0, 0, SCREEN_W, 14, TFT_DGRAY);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_DGRAY);
  tft.setCursor(2, 3);
  tft.print("CONNECT");

  if (vCount == 0) {
    // Still scanning
    static int spF = 0; static unsigned long spT = 0;
    if (millis()-spT > 150) { spF=(spF+1)%4; spT=millis(); }
    const char* sp[] = {"-","\\","|","/"};
    tft.setCursor(SCREEN_W-10, 3); tft.print(sp[spF]);
    drawCentered("Scanning...", 45, 1, TFT_WHITE);
    drawCentered("Please wait",  60, 1, TFT_GRAY);
    drawTriangleIcon(7, SCREEN_H-6, TFT_CYAN);
    tft.setTextColor(TFT_GRAY, TFT_BLACK);
    tft.setCursor(15, SCREEN_H-9); tft.print("Back");
    flushFramebuf();
    return;
  }

  // Device count in title
  tft.setTextColor(TFT_CYAN, TFT_DGRAY);
  String countStr = String(vCount) + " found";
  tft.setCursor(SCREEN_W-(int)(countStr.length()*6)-2, 3);
  tft.print(countStr);

  // Device list
  const int MAX_VIS = 7;
  const int ROW_H   = 14;
  const int ROW_Y   = 16;

  int start = selected - 3;
  if (start < 0) start = 0;
  if (start > vCount - MAX_VIS) start = max(0, vCount - MAX_VIS);

  for (int i = 0; i < MAX_VIS; i++) {
    int index = start + i;
    if (index >= vCount) break;
    bool isSaved = isSavedSlot(index);
    String label;
    if (isSaved) {
      label = getSavedDeviceName();
      if (label.length() > 14) label = label.substring(0, 14);
      label = "* " + label;
    } else {
      BTDeviceEntry* dev = getDevice(virtualToReal(index));
      if (!dev) continue;
      label = dev->name != "" ? dev->name : dev->address;
      if (label.length() > 18) label = label.substring(0, 18);
    }
    bool sel = (index == selected);
    if (sel) tft.fillRect(0, ROW_Y+i*ROW_H, SCREEN_W, ROW_H, TFT_DGRAY);
    uint16_t col = isSaved ? 0x07E0 : TFT_WHITE;
    if (sel) col = TFT_YELLOW;
    tft.setTextColor(col, sel ? TFT_DGRAY : TFT_BLACK);
    tft.setCursor(4, ROW_Y+i*ROW_H+3);
    tft.print(label);
  }

  // Hint bar
  tft.drawFastHLine(0, SCREEN_H-13, SCREEN_W, TFT_DGRAY);
  tft.fillRect(0, SCREEN_H-12, SCREEN_W, 12, TFT_BLACK);
  drawJoystickIcon(7,   SCREEN_H-6, TFT_CYAN);
  tft.setTextColor(TFT_GRAY, TFT_BLACK); tft.setCursor(15, SCREEN_H-9); tft.print("Nav");
  drawSquareIcon(52,    SCREEN_H-6, TFT_CYAN);
  tft.setTextColor(TFT_GRAY, TFT_BLACK); tft.setCursor(60, SCREEN_H-9); tft.print("Connect");
  drawTriangleIcon(107, SCREEN_H-6, TFT_CYAN);
  tft.setTextColor(TFT_GRAY, TFT_BLACK); tft.setCursor(115, SCREEN_H-9); tft.print("^");

  flushFramebuf();
}

// ─────────────────────────────────────────────────────────────
// PID page
// ─────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────
// Dial helpers
// ─────────────────────────────────────────────────────────────

// Dial geometry — constants moved into drawDial() as local variables

// ── Dial color helpers ───────────────────────────────────────────
#define COLOR_NAVY      0x0000   // pure black background
#define COLOR_ARCBG     0x18E3   // dark gray arc background
#define COLOR_ARC_BLUE  0x07FF   // cyan-blue (cold/normal)
#define COLOR_ARC_YEL   0xFFE0   // yellow (caution)
#define COLOR_ARC_RED   0xF800   // red (warn)
#define COLOR_DIMGRAY   0x4208   // dim gray
#define COLOR_LTGRAY    0xC618   // light gray

static uint16_t arcSegColor(float segPct, float warnPct) {
  if (warnPct <= 0.0f) {
    if      (segPct < 0.5f) return COLOR_ARC_BLUE;
    else if (segPct < 0.8f) return COLOR_ARC_YEL;
    else                     return COLOR_ARC_RED;
  }
  if      (segPct < warnPct * 0.7f) return COLOR_ARC_BLUE;
  else if (segPct < warnPct)         return COLOR_ARC_YEL;
  else                                return COLOR_ARC_RED;
}

static void drawDial(float pct, float warnPct,
                     const char* label, const char* valStr,
                     const char* unit,  bool hasData,
                     int pageNum, int pageTotal, bool regenActive) {

  // ── Option A: Horizontal bar gauge ───────────────────────────
  // Layout:
  //   [LABEL]              [N/N]   ← row 0
  //   ┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄   ← separator
  //   [████████░░░░░░░░░░░░░░░░]   ← bar y=20, h=12
  //   [min              max   ]   ← scale labels
  //                               ← spacer
  //       [   1 2 5 0   ]          ← huge value, size 4
  //       [    unit     ]          ← small unit

  tft.fillScreen(TFT_BLACK);

  // Regen flash border
  if (regenActive && (millis() / 400) % 2 == 0)
    tft.drawRect(0, 0, SCREEN_W, SCREEN_H, COLOR_ARC_RED);

  // ── Header row ───────────────────────────────────────────────
  tft.setTextSize(1);
  tft.setTextColor(COLOR_LTGRAY, TFT_BLACK);
  int16_t tx, ty; uint16_t tw, th;  // for label bounds
  tft.getTextBounds(label, 0, 0, &tx, &ty, &tw, &th);
  tft.setCursor((SCREEN_W - tw) / 2, 3);
  tft.print(label);

  tft.setTextColor(COLOR_DIMGRAY, TFT_BLACK);
  String pageStr = String(pageNum) + "/" + String(pageTotal);
  tft.setCursor(SCREEN_W - (int)(pageStr.length() * 6) - 2, 3);
  tft.print(pageStr);

  // Connection status dot — top left, green=connected red=not
  tft.fillCircle(5, 7, 3, isBTConnected() ? 0x07E0 : 0xF800);

  tft.drawFastHLine(4, 14, SCREEN_W - 8, COLOR_DIMGRAY);

  // ── Value — large, below header ──────────────────────────────
  // Layout: header(14) | value(18-58) | unit(60) | bar(72-96) | hint(118)
  uint16_t valColor = hasData ? COLOR_ARC_YEL : COLOR_DIMGRAY;
  tft.setTextSize(4);
  tft.setTextColor(valColor, TFT_BLACK);
  tft.getTextBounds(valStr, 0, 0, &tx, &ty, &tw, &th);
  if (tw > SCREEN_W - 8) {
    tft.setTextSize(3);
    tft.getTextBounds(valStr, 0, 0, &tx, &ty, &tw, &th);
  }
  tft.setCursor((SCREEN_W - tw) / 2, 18);
  tft.print(valStr);

  // ── Unit ──────────────────────────────────────────────────────
  if (strlen(unit) > 0) {
    tft.setTextSize(1);
    tft.setTextColor(COLOR_DIMGRAY, TFT_BLACK);
    tft.getTextBounds(unit, 0, 0, &tx, &ty, &tw, &th);
    tft.setCursor((SCREEN_W - tw) / 2, 62);
    tft.print(unit);
  }

  // ── Horizontal bar — below value ─────────────────────────────
  const int BAR_X = 6;
  const int BAR_Y = 74;
  const int BAR_W = SCREEN_W - 12;
  const int BAR_H = 22;

  tft.fillRect(BAR_X, BAR_Y, BAR_W, BAR_H, 0x1082);
  tft.drawRect(BAR_X - 1, BAR_Y - 1, BAR_W + 2, BAR_H + 2, COLOR_DIMGRAY);

  if (hasData && pct > 0.0f) {
    int filled = (int)(BAR_W * constrain(pct, 0.0f, 1.0f));
    for (int x = 0; x < filled; x++) {
      float segPct = (float)x / BAR_W;
      uint16_t col = arcSegColor(segPct, warnPct);
      tft.drawFastVLine(BAR_X + x, BAR_Y, BAR_H, col);
    }
  }

  if (warnPct > 0.0f) {
    int warnX = BAR_X + (int)(BAR_W * warnPct);
    tft.drawFastVLine(warnX, BAR_Y - 2, BAR_H + 4, 0xFFFF);
  }

  // ── Corner icons ──────────────────────────────────────────────
  // Centered hint: square icon + "Menu"
  tft.setTextSize(1);
  tft.setTextColor(COLOR_DIMGRAY, TFT_BLACK);
  drawSquareIcon(SCREEN_W/2 - 14, SCREEN_H - 6, COLOR_DIMGRAY);
  tft.setCursor(SCREEN_W/2 - 6, SCREEN_H - 10);
  tft.print("Menu");
}

// ─────────────────────────────────────────────────────────────
// showPIDPage — uses dial for numeric, text for boolean
// ─────────────────────────────────────────────────────────────

void ui_gauge(int pidIndex) {
  // No PIDs active — show placeholder instead of blank/crash
  if (getActivePIDCount() == 0) {
    tft.fillScreen(TFT_BLACK);
    tft.fillRect(0, 0, SCREEN_W, 14, TFT_DGRAY);
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_DGRAY);
    tft.setCursor(2, 3); tft.print("GAUGE");
    tft.fillCircle(5, 7, 3, isBTConnected() ? 0x07E0 : 0xF800);
    tft.drawFastHLine(4, 14, SCREEN_W - 8, COLOR_DIMGRAY);
    drawCentered("No PIDs selected", 50, 1, TFT_GRAY);
    drawCentered("Open Menu to", 65, 1, TFT_GRAY);
    drawCentered("configure PIDs", 75, 1, TFT_GRAY);
    tft.setTextSize(1); tft.setTextColor(COLOR_DIMGRAY, TFT_BLACK);
    drawSquareIcon(SCREEN_W/2 - 14, SCREEN_H - 6, COLOR_DIMGRAY);
    tft.setCursor(SCREEN_W/2 - 6, SCREEN_H - 10); tft.print("Menu");
    flushFramebuf();
    return;
  }

  if (pidIndex < 0 || pidIndex >= PID_COUNT) return;

  const PIDDef& p = PIDS[pidIndex];
  OBDValues&    v = getOBDValues();

  float val    = v.values[pidIndex];
  bool hasData = v.hasData[pidIndex];

  // Regen check
  bool regenActive = false;
  for (int i = 0; i < PID_COUNT; i++) {
    if (PIDS[i].regenFlag && v.hasData[i] && v.values[i] != 0.0) {
      regenActive = true; break;
    }
  }

  String title = (regenActive && !PIDS[pidIndex].regenFlag)
                 ? String("! ") + p.name : p.name;

  // Compute warnPct — 0 if no warn defined
  float warnPct = 0.0f;
  if (p.warn > 0.0f && p.valMax > p.valMin) {
    warnPct = (p.warn - p.valMin) / (p.valMax - p.valMin);
  }

  // Active page position
  int aPos = 0, aTotal = 0;
  for (int i = 0; i < PID_COUNT; i++) {
    if (!isPIDActive(i)) continue;
    aTotal++;
    if (i <= pidIndex) aPos = aTotal;
  }

  if (p.isBoolean) {
    // Boolean PIDs — navy background, large ON/OFF
    tft.fillScreen(COLOR_NAVY);
    if (regenActive && (millis() / 400) % 2 == 0)
      tft.drawRect(0, 0, SCREEN_W, SCREEN_H, COLOR_ARC_RED);
    tft.setTextSize(1);
    tft.setTextColor(COLOR_LTGRAY, COLOR_NAVY);
    int16_t tx, ty; uint16_t tw, th;
    tft.getTextBounds(title.c_str(), 0, 0, &tx, &ty, &tw, &th);
    tft.setCursor((SCREEN_W - tw) / 2, 3);
    tft.print(title.c_str());
    tft.setTextColor(COLOR_DIMGRAY, COLOR_NAVY);
    String pageStr = String(aPos) + "/" + String(aTotal);
    tft.setCursor(SCREEN_W - (int)(pageStr.length() * 6) - 2, 3);
    tft.print(pageStr);
    const char* boolStr = !hasData ? "---" : (val != 0.0 ? "ON" : "OFF");
    uint16_t boolColor  = !hasData ? COLOR_DIMGRAY : (val != 0.0 ? COLOR_ARC_RED : 0x07E0);
    drawCentered(boolStr, 50, 3, boolColor);
    drawSquareIcon(SCREEN_W/2 - 14, SCREEN_H - 6, COLOR_DIMGRAY);
    tft.setTextSize(1); tft.setTextColor(COLOR_DIMGRAY, TFT_BLACK);
    tft.setCursor(SCREEN_W/2 - 6, SCREEN_H - 10); tft.print("Menu");
  } else {
    // Numeric PIDs — arc dial
    float pct = 0.0f;
    if (hasData) {
      float clamped = constrain(val, p.valMin, p.valMax);
      pct = (clamped - p.valMin) / (p.valMax - p.valMin);
    }
    String valStr;
    if      (!hasData)          valStr = "---";
    else if (p.valMax >= 1000)  valStr = String((int)val);
    else                        valStr = String(val, 1);

    drawDial(pct, warnPct, title.c_str(), valStr.c_str(), p.unit,
             hasData, aPos, aTotal, regenActive);
  }

  // RGB LEDs — reflect current page value vs warn threshold
  {
    OBDValues& lv = getOBDValues();
    if (lv.hasData[pidIndex] && !p.isBoolean) {
      float clamped = constrain(lv.values[pidIndex], p.valMin, p.valMax);
      float pagePct = (clamped - p.valMin) / (p.valMax - p.valMin);
      ui_leds(pagePct, warnPct);
    } else if (p.isBoolean && lv.hasData[pidIndex]) {
      // Boolean: all red if ON, all off if OFF
      if (lv.values[pidIndex] != 0.0f)
        rgb_write_all(40,0,0, 40,0,0, 40,0,0, 40,0,0, 40,0,0);
      else
        ui_leds_off();
    }
  }

  flushFramebuf();
}

// Probe state — index of PID currently being probed (-1 = done)
// Managed by the main loop, read here for display
// probe state now managed by obd.cpp background probe

void ui_menuPIDs(int cursor) {
  tft.fillScreen(TFT_BLACK);

  // Title bar
  tft.fillRect(0, 0, SCREEN_W, 14, TFT_DGRAY);
  tft.drawFastHLine(0, 14, SCREEN_W, TFT_GRAY);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(1);
  tft.setCursor(2, 3);
  tft.print("SELECT PIDS");

  // Status: active PID count (bg probe removed)
  tft.setTextColor(TFT_GRAY);
  tft.setCursor(74, 3);
  tft.print(String(getActivePIDCount()) + " active");

  // List — 7 rows
  const int MAX_VISIBLE = 7;
  const int ROW_H       = 13;
  const int ROW_START   = 18;

  int total = PID_COUNT;  // show ALL pids
  int start = cursor - 2;
  if (start < 0) start = 0;
  if (start > total - MAX_VISIBLE) start = max(0, total - MAX_VISIBLE);

  for (int i = 0; i < MAX_VISIBLE; i++) {
    int idx = start + i;
    if (idx >= total) break;

    bool active   = isPIDActive(idx);
    bool isCursor = (idx == cursor);

    uint16_t bg = isCursor ? TFT_DGRAY : TFT_BLACK;

    if (isCursor) tft.fillRect(0, ROW_START + i * ROW_H, SCREEN_W, ROW_H, TFT_DGRAY);

    // Checkbox [x]/[ ] — 12px wide
    tft.setTextSize(1);
    tft.setTextColor(active ? TFT_GREEN : TFT_GRAY, bg);
    tft.setCursor(2, ROW_START + i * ROW_H + 2);
    tft.print(active ? "[x]" : "[ ]");

    // PID name — yellow on cursor, white otherwise
    uint16_t nameColor = isCursor ? TFT_YELLOW : TFT_WHITE;

    tft.setTextColor(nameColor, bg);
    tft.setCursor(22, ROW_START + i * ROW_H + 2);
    String label = String(PIDS[idx].name);
    if (label.length() > 13) label = label.substring(0, 13);
    tft.print(label);

    // Status on far right — always show live value if we have it, --- if not
    String status = "";
    uint16_t statusColor = TFT_DGRAY;

    OBDValues& v = getOBDValues();
    if (v.hasData[idx]) {
      if (PIDS[idx].isBoolean)
        status = v.values[idx] ? "ON" : "OFF";
      else if (PIDS[idx].valMax >= 1000)
        status = String((int)v.values[idx]);
      else
        status = String(v.values[idx], 0);
      if (status.length() > 6) status = status.substring(0, 6);
      statusColor = TFT_GREEN;
    } else {
      status = "---";
      statusColor = TFT_DGRAY;
    }

    // Right-align status at x=127
    int statusX = 127 - (status.length() * 6);
    tft.setTextColor(statusColor, bg);
    tft.setCursor(statusX, ROW_START + i * ROW_H + 2);
    tft.print(status);
  }

  // Hint bar
  tft.drawFastHLine(0, ROW_START + MAX_VISIBLE * ROW_H, SCREEN_W, TFT_DGRAY);
  drawJoystickIcon(10,  118, TFT_CYAN);
  tft.setTextSize(1); tft.setTextColor(TFT_GRAY, TFT_BLACK);
  tft.setCursor(20, 115); tft.print("Tog");
  drawHoldArc(52, 118, TFT_CYAN);
  tft.setCursor(62, 115); tft.print("OK");
  drawTriangleIcon(100, 118, TFT_CYAN);
  tft.setCursor(110, 115); tft.print("Quit");

  flushFramebuf();
}

// ─────────────────────────────────────────────────────────────
// PID scan progress — called from obd.cpp probe loop
// ─────────────────────────────────────────────────────────────

void ui_menuPIDProgress(int done, int total, const char* name) {
  tft.fillScreen(TFT_BLACK);

  drawTitle("Scanning PIDs");

  // PID name being probed
  tft.setTextSize(1);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  if (total > 0 && done < total) {
    drawCentered(name, 35, 1, TFT_CYAN);
  } else {
    drawCentered("Done!", 35, 1, TFT_GREEN);
  }

  // Progress bar
  int pct = (total > 0) ? (done * 100 / total) : 100;
  tft.drawRect(4, 55, 120, 10, TFT_WHITE);
  int filled = (int)(118.0f * pct / 100.0f);
  if (filled > 0) tft.fillRect(5, 56, filled, 8, TFT_CYAN);

  // Counter
  String counter = String(done) + " / " + String(total);
  drawCentered(counter.c_str(), 72, 1, TFT_GRAY);

  flushFramebuf();
}

// ─────────────────────────────────────────────────────────────
// Dispatcher
// ─────────────────────────────────────────────────────────────

extern int  gaugePage;
extern bool gaugeLocked;

void ui_updateLegacy(int state) {
  if (state == STATE_SCANNING)     { ui_menuConnect(getDeviceCount(), getSelectedIndex()); return; }
  if (state == STATE_AUTO_CONNECT) { ui_autoConnect();              return; }
  if (state == STATE_CONNECTING)   { ui_connecting();               return; }
  if (state == STATE_INIT_ELM)     { ui_connecting();               return; }
  if (state == STATE_PID_SCAN)     { return; }  // handled by input.cpp directly
  if (state == STATE_GAUGE) {
    static unsigned long lastGaugeUpdate = 0;
    if (millis() - lastGaugeUpdate < 200) return;
    lastGaugeUpdate = millis();

    // No active PIDs — caller (ino) redirects to PID selector
    if (getActivePIDCount() == 0) return;

    // Skip to first active page if gaugePage is inactive
    if (!isPIDActive(gaugePage)) {
      for (int i = 0; i < PID_COUNT; i++) {
        if (isPIDActive(i)) { gaugePage = i; break; }
      }
    }

    ui_gauge(gaugePage);
  }
}

// ─────────────────────────────────────────────────────────────
// ui_menu — top-level menu screen
// Items: PIDs / Connect / Settings / Defaults / Back
// ─────────────────────────────────────────────────────────────
void ui_menu(int selection, bool connected) {
  tft.fillScreen(TFT_BLACK);

  // Title bar
  tft.fillRect(0, 0, SCREEN_W, 14, TFT_DGRAY);
  tft.drawFastHLine(0, 14, SCREEN_W, TFT_GRAY);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_DGRAY);
  tft.setCursor(2, 3);
  tft.print("MENU");

  // Connection status top-right
  tft.setTextColor(connected ? TFT_GREEN : TFT_RED, TFT_DGRAY);
  tft.setCursor(SCREEN_W - 42, 3);
  tft.print(connected ? "ONLINE" : "OFFLN");

  // Menu items
  const char* items[] = { "PIDs", "Connect", "Settings", "Defaults" };
  const int   ITEM_COUNT = 4;
  const int   ROW_H   = 20;
  const int   ROW_Y   = 20;

  for (int i = 0; i < ITEM_COUNT; i++) {
    bool sel = (i == selection);
    if (sel) tft.fillRect(0, ROW_Y + i * ROW_H, SCREEN_W, ROW_H, TFT_DGRAY);
    tft.setTextSize(1);
    tft.setTextColor(sel ? TFT_YELLOW : TFT_WHITE, sel ? TFT_DGRAY : TFT_BLACK);
    tft.setCursor(10, ROW_Y + i * ROW_H + 4);
    tft.print(sel ? "> " : "  ");
    tft.print(items[i]);
  }

  // Hint bar — 3 slots × 42px = 126px, fits within 128px
  tft.drawFastHLine(0, SCREEN_H - 13, SCREEN_W, TFT_DGRAY);
  tft.fillRect(0, SCREEN_H - 12, SCREEN_W, 12, TFT_BLACK);
  // Slot 1 (x=2): joystick + "Up/Dn"
  drawJoystickIcon(7,  SCREEN_H - 6, TFT_CYAN);
  tft.setTextColor(TFT_GRAY, TFT_BLACK); tft.setCursor(15, SCREEN_H - 9);
  tft.print("Up/Dn");
  // Slot 2 (x=44): square + "Sel"
  drawSquareIcon(49, SCREEN_H - 6, TFT_CYAN);
  tft.setTextColor(TFT_GRAY, TFT_BLACK); tft.setCursor(57, SCREEN_H - 9);
  tft.print("Sel");
  // Slot 3 (x=86): triangle + "Back"
  drawTriangleIcon(91, SCREEN_H - 6, TFT_CYAN);
  tft.setTextColor(TFT_GRAY, TFT_BLACK); tft.setCursor(99, SCREEN_H - 9);
  tft.print("Back");

  flushFramebuf();
}

// ─────────────────────────────────────────────────────────────
// ui_menuSettings — stub, shows placeholder
// ─────────────────────────────────────────────────────────────
void ui_menuSettings(int /*cursor*/) {
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, SCREEN_W, 14, TFT_DGRAY);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_DGRAY);
  tft.setCursor(2, 3); tft.print("SETTINGS");
  drawCentered("Coming soon", 55, 1, TFT_GRAY);
  tft.setTextSize(1); tft.setTextColor(TFT_GRAY, TFT_BLACK);
  tft.setCursor(2, SCREEN_H - 9); tft.print("^:back");
  flushFramebuf();
}

// ─────────────────────────────────────────────────────────────
// ui_menuDefaults — stub, shows placeholder
// ─────────────────────────────────────────────────────────────
void ui_menuDefaults(int /*cursor*/) {
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, SCREEN_W, 14, TFT_DGRAY);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_DGRAY);
  tft.setCursor(2, 3); tft.print("DEFAULTS");
  drawCentered("Coming soon", 55, 1, TFT_GRAY);
  tft.setTextSize(1); tft.setTextColor(TFT_GRAY, TFT_BLACK);
  tft.setCursor(2, SCREEN_H - 9); tft.print("^:back");
  flushFramebuf();
}
