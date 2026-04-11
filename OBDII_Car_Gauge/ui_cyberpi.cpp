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
// Theme — all UI colors go through these variables
// Dark mode (default): black bg, white text
// Light mode:          white bg, dark text
// ─────────────────────────────────────────────────────────────
static bool themeDark = true;  // loaded from NVS in ui_init()

// Theme color accessors — use these everywhere instead of TFT_BLACK/WHITE directly
static inline uint16_t T_BG()       { return themeDark ? 0x0000 : 0xFFFF; }  // page background
static inline uint16_t T_FG()       { return themeDark ? 0xFFFF : 0x0000; }  // primary text
static inline uint16_t T_BAR()      { return themeDark ? 0x2104 : 0xC618; }  // title bar
static inline uint16_t T_BAR_TXT()  { return themeDark ? 0xFFFF : 0x0000; }  // title bar text
static inline uint16_t T_SEP()      { return themeDark ? 0x4208 : 0x8410; }  // separator lines
static inline uint16_t T_DIM()      { return themeDark ? 0x4208 : 0x8410; }  // dim text / icons
static inline uint16_t T_DARKER()   { return themeDark ? 0x2104 : 0xC618; }  // darker hint text
static inline uint16_t T_SEL()      { return themeDark ? 0x2104 : 0xC618; }  // selected row bg
static inline uint16_t T_OVERLAY()  { return themeDark ? 0x0821 : 0x9CF3; }  // overlay bg

void ui_setTheme(bool dark) {
  themeDark = dark;
  Preferences prefs;
  prefs.begin("theme", false);
  prefs.putBool("dark", dark);
  prefs.end();
}

bool ui_isDarkTheme() { return themeDark; }

static void loadTheme() {
  Preferences prefs;
  prefs.begin("theme", true);
  themeDark = prefs.getBool("dark", true);  // default dark
  prefs.end();
}

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
  loadTheme();  // load dark/light preference from NVS
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
  tft.fillRect(0, 0, SCREEN_W, 14, T_BAR());
  tft.drawFastHLine(0, 14, SCREEN_W, T_SEP());
  tft.setTextColor(T_FG());
  tft.setTextSize(1);
  tft.setCursor(2, 3);
  tft.print(title);
}

static void drawCentered(const char* text, int y, int size, uint16_t color) {
  tft.setTextSize(size);
  tft.setTextColor(color, T_BG());
  int16_t x1, y1;
  uint16_t w, h;
  tft.getTextBounds(text, 0, y, &x1, &y1, &w, &h);
  tft.setCursor((SCREEN_W - w) / 2, y);
  tft.print(text);
}

static void drawProgressBar(int x, int y, int w, int h, int pct) {
  tft.drawRect(x, y, w, h, T_SEP());
  int filled = (int)((w - 2) * pct / 100.0);
  if (filled > 0)
    tft.fillRect(x + 1, y + 1, filled, h - 2, TFT_CYAN);
  if (filled < w - 2)
    tft.fillRect(x + 1 + filled, y + 1, w - 2 - filled, h - 2, T_BG());
}

// ─────────────────────────────────────────────────────────────
// Public display utilities — must be after tft declaration
// ─────────────────────────────────────────────────────────────

// Public flush — forces framebuffer to screen
void ui_flush() { flushFramebuf(); }

// Scanning overlay — call before startScan() to show feedback
void ui_scanOverlay() {
  // Draws on TOP of the current connect page content.
  // Only fills the list area (y=16 to y=99), leaving title and hints visible.
  // Animation: diagonal bar sweeping across the overlay box.
  static int          spF = 0;
  static unsigned long spT = 0;
  if (millis() - spT > 60) { spF = (spF + 1) % 32; spT = millis(); }

  const int OX = 0, OY = 16, OW = SCREEN_W, OH = 83;  // overlay area

  // Dark semi-transparent background
  tft.fillRect(OX, OY, OW, OH, T_OVERLAY());

  // Diagonal bar — moves across the box
  // Bar is a diagonal stripe 12px wide, sweeping left to right
  int barPos = (spF * (OW + OH)) / 32 - OH;  // -OH to OW range
  for (int y = OY; y < OY + OH; y++) {
    int x0 = barPos + (y - OY);  // diagonal: x shifts with y
    // Draw a 14px wide bright stripe
    for (int dx = 0; dx < 14; dx++) {
      int x = x0 + dx;
      if (x >= OX && x < OX + OW) {
        // Brightness falls off at edges of stripe
        uint16_t col = (dx < 2 || dx > 11) ? 0x2104 :
                       (dx < 4 || dx > 9)  ? 0x4208 :
                       (dx < 6 || dx > 7)  ? 0x8410 : 0xC618;
        tft.drawPixel(x, y, col);
      }
    }
  }

  // Centered label
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, 0x0821);
  // "Scanning..." centered in overlay
  int lx = OX + (OW - 11*6) / 2;
  int ly = OY + OH/2 - 4;
  // Clear text background
  tft.fillRect(lx - 4, ly - 2, 11*6 + 8, 12, 0x0821);
  tft.setCursor(lx, ly);
  tft.print("Scanning...");

  flushFramebuf();
}

void ui_connectOverlay() {
  // Same diagonal bar animation as scan, but labeled "Connecting..."
  static int           cpF = 0;
  static unsigned long cpT = 0;
  if (millis() - cpT > 60) { cpF = (cpF + 1) % 32; cpT = millis(); }

  const int OX = 0, OY = 16, OW = SCREEN_W, OH = 83;
  int barPos = (cpF * (OW + OH)) / 32 - OH;
  tft.fillRect(OX, OY, OW, OH, T_OVERLAY());
  for (int y = OY; y < OY + OH; y++) {
    int x0 = barPos + (y - OY);
    for (int dx = 0; dx < 14; dx++) {
      int x = x0 + dx;
      if (x >= OX && x < OX + OW) {
        uint16_t col = (dx < 2 || dx > 11) ? 0x2104 :
                       (dx < 4 || dx > 9)  ? 0x4208 :
                       (dx < 6 || dx > 7)  ? 0x8410 : 0xC618;
        tft.drawPixel(x, y, col);
      }
    }
  }
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, 0x0821);
  int lx = OX + (OW - 13*6) / 2;  // "Connecting..." = 13 chars
  int ly = OY + OH/2 - 4;
  tft.fillRect(lx - 4, ly - 2, 13*6 + 8, 12, 0x0821);
  tft.setCursor(lx, ly);
  tft.print("Connecting...");
  flushFramebuf();
}

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
// Icons — all 10px tall, centered on cy
// ─────────────────────────────────────────────────────────────

static void drawJoystickIcon(int cx, int cy, uint16_t color) {
  tft.drawCircle(cx, cy, 4, color);
  tft.fillCircle(cx, cy, 2, color);
  tft.drawPixel(cx,   cy-4, color);
  tft.drawPixel(cx,   cy+4, color);
  tft.drawPixel(cx-4, cy,   color);
  tft.drawPixel(cx+4, cy,   color);
}

static void drawSquareIcon(int cx, int cy, uint16_t color) {
  tft.drawRect(cx-4, cy-4, 9, 9, color);
  tft.drawRect(cx-3, cy-3, 7, 7, color);
}

static void drawTriangleIcon(int cx, int cy, uint16_t color) {
  tft.drawLine(cx,   cy-4, cx-4, cy+4, color);
  tft.drawLine(cx-4, cy+4, cx+4, cy+4, color);
  tft.drawLine(cx+4, cy+4, cx,   cy-4, color);
}

static void drawCenterDotIcon(int cx, int cy, uint16_t color) {
  tft.drawCircle(cx, cy, 4, color);
  tft.fillCircle(cx, cy, 2, color);
}

// ─────────────────────────────────────────────────────────────
// drawPageHints — unified 2-line footer used on every page
//
// Pass up to 4 hint slots. Each slot = { icon, label }.
// icon: "joy"=joystick "sq"=square "tri"=triangle "dot"=center dot
// Slots are distributed evenly across 128px.
// Line 1 (y=107): separator line
// Line 2 (y=110): labels (gray text, centered under icon)
// Line 3 (y=120): icons
// ─────────────────────────────────────────────────────────────

struct HintSlot { const char* icon; const char* label; };

static void drawPageHints(HintSlot slots[], int count) {
  // Footer: separator at y=99, icon at y=107, label at y=118
  // Total footer height: 128-99 = 29px. Icon=10px, gap=1px, label=8px = 19px. Fits.
  const uint16_t BG      = T_BG();
  const uint16_t SEP_COL = T_SEP();
  const uint16_t ICO_COL = T_DIM();
  const uint16_t TXT_COL = T_DARKER();

  tft.drawFastHLine(0, 99, SCREEN_W, T_SEP());
  tft.fillRect(0, 100, SCREEN_W, SCREEN_H - 100, T_BG());

  if (count < 1) return;

  int slotW = SCREEN_W / count;

  for (int i = 0; i < count; i++) {
    int cx = slotW * i + slotW / 2;

    // Icon at y=107
    const char* ic = slots[i].icon;
    if      (strcmp(ic, "joy") == 0) drawJoystickIcon(cx,  107, ICO_COL);
    else if (strcmp(ic, "sq")  == 0) drawSquareIcon(cx,    107, ICO_COL);
    else if (strcmp(ic, "tri") == 0) drawTriangleIcon(cx,  107, ICO_COL);
    else if (strcmp(ic, "dot") == 0) drawCenterDotIcon(cx, 107, ICO_COL);

    // Label at y=118
    tft.setTextSize(1);
    tft.setTextColor(TXT_COL, BG);
    int labelW = strlen(slots[i].label) * 6;
    int labelX = cx - labelW / 2;
    if (labelX < slotW * i) labelX = slotW * i;
    tft.setCursor(labelX, 118);
    tft.print(slots[i].label);
  }
}

// Convenience wrappers for common layouts
static void drawHintBar(const char* i1, const char* l1,
                        const char* i2, const char* l2) {
  HintSlot s[] = {{i1,l1},{i2,l2}};
  drawPageHints(s, 2);
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

  tft.fillScreen(T_BG());

  // Spinner
  tft.setTextSize(2);
  tft.setTextColor(TFT_CYAN, T_BG());
  tft.setCursor(58, 8);
  tft.print(spinner[frame]);

  tft.drawFastHLine(0, 28, SCREEN_W, T_SEP());

  drawCentered("Reconnecting to", 33, 1, T_DIM());

  String name = getSavedDeviceName();
  if (name == "") name = "Saved device";
  if (name.length() <= 10) {
    drawCentered(name.c_str(), 50, 2, T_FG());
  } else {
    drawCentered(name.substring(0, 16).c_str(), 46, 1, T_FG());
    if (name.length() > 16)
      drawCentered(name.substring(16, 32).c_str(), 58, 1, T_FG());
  }

  // Progress bar — fills left to right over 5s
  int filled = (int)(118.0f * (pct / 100.0f));
  tft.drawRect(4, 80, 120, 8, T_SEP());
  if (filled > 0)   tft.fillRect(5, 81, filled, 6, TFT_CYAN);
  if (filled < 118) tft.fillRect(5 + filled, 81, 118 - filled, 6, T_BG());

  // Status hint at bottom
  tft.setTextSize(1);
  tft.setTextColor(T_DIM(), T_BG());
  tft.setCursor(2, 112);
  tft.print(pct < 20 ? "Waiting..." : pct < 80 ? "Connecting..." : "Almost...");

  flushFramebuf();
}

void ui_connecting() {
  static int           frame     = 0;
  static unsigned long lastFrame = 0;
  if (millis() - lastFrame > 200) { frame = (frame + 1) % 8; lastFrame = millis(); }
  const char* spinner[] = { "|", "/", "-", "\\", "|", "/", "-", "\\" };

  tft.fillScreen(T_BG());

  tft.setTextSize(2);
  tft.setTextColor(TFT_YELLOW, T_BG());
  tft.setCursor(58, 8);
  tft.print(spinner[frame]);

  tft.drawFastHLine(0, 28, SCREEN_W, T_SEP());

  drawCentered("Connecting", 36, 1, T_DIM());

  String name = getSavedDeviceName();
  if (name == "") name = "OBD Adapter";
  // Use size 2 for short names, size 1 for longer ones
  if (name.length() <= 10) {
    drawCentered(name.c_str(), 52, 2, T_FG());
  } else {
    // Split to two lines at size 1
    drawCentered(name.substring(0, 16).c_str(), 48, 1, T_FG());
    if (name.length() > 16)
      drawCentered(name.substring(16, 32).c_str(), 60, 1, T_FG());
  }

  tft.drawFastHLine(0, 98, SCREEN_W, T_SEP());
  drawCentered("Please wait...", 104, 1, T_DIM());

  flushFramebuf();
}

static void showWaitingData() {
  static int           frame     = 0;
  static unsigned long lastFrame = 0;
  if (millis() - lastFrame > 200) { frame = (frame + 1) % 8; lastFrame = millis(); }
  const char* spinner[] = { "|", "/", "-", "\\", "|", "/", "-", "\\" };

  tft.fillScreen(T_BG());

  tft.setTextSize(2);
  tft.setTextColor(TFT_GREEN, T_BG());
  tft.setCursor(58, 8);
  tft.print(spinner[frame]);

  tft.drawFastHLine(0, 28, SCREEN_W, T_SEP());

  drawCentered("Connected!", 36, 1, T_DIM());

  String name = getSavedDeviceName();
  if (name == "") name = "OBD Adapter";
  if (name.length() <= 10) {
    drawCentered(name.c_str(), 52, 2, T_FG());
  } else {
    drawCentered(name.substring(0, 16).c_str(), 48, 1, T_FG());
    if (name.length() > 16)
      drawCentered(name.substring(16, 32).c_str(), 60, 1, T_FG());
  }

  tft.drawFastHLine(0, 78, SCREEN_W, T_SEP());
  drawCentered("Getting data...", 84, 1, T_DIM());

  flushFramebuf();
}

// ─────────────────────────────────────────────────────────────
// Virtual device list helpers
// ─────────────────────────────────────────────────────────────

// ── Virtual list layout ─────────────────────────────────────
// index 0        → SAVED device (if exists) — hold to forget
// index 1..N     → scanned BLE devices (deviceList[index-offset])
// ─────────────────────────────────────────────────────────────

// scanLastSelected/scanLastCount removed — connect page always redraws

void ui_resetScanDisplay() {}  // kept for compat, no-op

void ui_menuConnect(int /*unused*/, int selected) {
  // Virtual list:
  //   index 0             = [ SCAN ]
  //   index 1..savedCount = saved devices  (BTSavedDevice)
  //   index savedCount+1.. = scan results not already saved (BTDeviceEntry)

  int savedCount = getSavedDeviceCount();
  int scanCount  = getDeviceCount();

  // Count scan results not already in saved list
  int newScanCount = 0;
  bool scanIsNew[MAX_SCAN_DEVICES] = {};
  for (int i = 0; i < scanCount; i++) {
    BTDeviceEntry* se = getDevice(i);
    if (!se) continue;
    bool alreadySaved = false;
    for (int j = 0; j < savedCount; j++) {
      BTSavedDevice* sd = getSavedDevice(j);
      if (sd && sd->mac == se->address) { alreadySaved = true; break; }
    }
    if (!alreadySaved) { scanIsNew[i] = true; newScanCount++; }
  }

  int totalItems = 1 + savedCount + newScanCount;

  tft.fillScreen(T_BG());

  // Title bar
  tft.fillRect(0, 0, SCREEN_W, 14, T_BAR());
  tft.setTextSize(1);
  tft.setTextColor(T_BAR_TXT(), T_BAR());
  tft.setCursor(2, 3);
  tft.print("CONNECT");

  // Show counts in title
  tft.setTextColor(TFT_CYAN, T_BAR());
  String info = "";
  if (savedCount > 0) info += String(savedCount) + "S";
  if (newScanCount > 0) info += (info.length() > 0 ? " " : "") + String(newScanCount) + "N";
  if (info.length() > 0) {
    tft.setCursor(SCREEN_W - (int)(info.length()*6) - 2, 3);
    tft.print(info);
  }

  // Device list
  const int MAX_VIS = 6;
  const int ROW_H   = 14;
  const int ROW_Y   = 16;

  // First row always: [ SCAN ]
  bool scanSel = (selected == 0);
  if (scanSel) tft.fillRect(0, ROW_Y, SCREEN_W, ROW_H, T_SEL());
  tft.setTextColor(TFT_CYAN, scanSel ? T_SEL() : T_BG());
  tft.setCursor(4, ROW_Y + 3);
  tft.print(scanSel ? "> [ SCAN ]" : "  [ SCAN ]");

  // If nothing known yet — show permanent message below SCAN item
  if (savedCount == 0 && newScanCount == 0) {
    tft.setTextSize(1);
    tft.setTextColor(T_DIM(), T_BG());
    tft.setCursor(4, ROW_Y + ROW_H + 6);
    tft.print("No devices found.");
    tft.setCursor(4, ROW_Y + ROW_H + 18);
    tft.print("Press O to scan.");
  { HintSlot s[] = {{"joy","Nav"},{"tri","Back"}}; drawPageHints(s, 2); }
    flushFramebuf();
    return;
  }

  int start = selected - 1;
  if (start < 1) start = 1;
  if (start > totalItems - MAX_VIS + 1) start = max(1, totalItems - MAX_VIS + 1);

  // Draw rows 1..totalItems-1 (SCAN row 0 already drawn above)
  for (int i = 1; i < MAX_VIS; i++) {
    int index = start + i - 1;
    if (index < 1 || index >= totalItems) break;

    bool sel = (index == selected);
    if (sel) tft.fillRect(0, ROW_Y+i*ROW_H, SCREEN_W, ROW_H, T_SEL());

    if (index <= savedCount) {
      // Saved device
      int di = index - 1;
      BTSavedDevice* dev = getSavedDevice(di);
      if (!dev) continue;
      bool isDefault = (di == getDefaultDeviceIndex());
      String label = dev->name != "" ? dev->name : dev->mac;
      if (label.length() > 14) label = label.substring(0, 14);
      String marker = isDefault ? "[*] " : "[ ] ";
      uint16_t col = isDefault ? 0x07E0 : TFT_WHITE;
      if (sel) col = TFT_YELLOW;
      tft.setTextColor(col, sel ? T_SEL() : T_BG());
      tft.setCursor(4, ROW_Y+i*ROW_H+3);
      tft.print(marker + label);

    } else {
      // New scan result (not saved)
      int ni = index - 1 - savedCount;
      int found = -1, cnt = 0;
      for (int j = 0; j < scanCount; j++) {
        if (scanIsNew[j]) { if (cnt == ni) { found = j; break; } cnt++; }
      }
      if (found < 0) continue;
      BTDeviceEntry* dev = getDevice(found);
      if (!dev) continue;
      String label = dev->name != "" ? dev->name : dev->address;
      if (label.length() > 16) label = label.substring(0, 16);
      uint16_t col = sel ? TFT_YELLOW : 0x8410;
      tft.setTextColor(col, sel ? T_SEL() : T_BG());
      tft.setCursor(4, ROW_Y+i*ROW_H+3);
      tft.print("  " + label);
    }
  }

  { HintSlot s[] = {{"joy","Nav"},{"tri","Back"}}; drawPageHints(s, 2); }

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
#define COLOR_ARC_YEL   0xFFE0   // yellow (caution) — only use in dark mode
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

  tft.fillScreen(T_BG());

  // Regen flash border
  if (regenActive && (millis() / 400) % 2 == 0)
    tft.drawRect(0, 0, SCREEN_W, SCREEN_H, COLOR_ARC_RED);

  // ── Header row ───────────────────────────────────────────────
  tft.setTextSize(1);
  tft.setTextColor(T_FG(), T_BG());
  int16_t tx, ty; uint16_t tw, th;  // for label bounds
  tft.getTextBounds(label, 0, 0, &tx, &ty, &tw, &th);
  tft.setCursor((SCREEN_W - tw) / 2, 3);
  tft.print(label);

  tft.setTextColor(T_DIM(), T_BG());
  String pageStr = String(pageNum) + "/" + String(pageTotal);
  tft.setCursor(SCREEN_W - (int)(pageStr.length() * 6) - 2, 3);
  tft.print(pageStr);

  // Connection status dot — top left, green=connected red=not
  tft.fillCircle(5, 7, 3, isBTConnected() ? 0x07E0 : 0xF800);

  tft.drawFastHLine(4, 14, SCREEN_W - 8, T_SEP());

  // ── Value — large, below header ──────────────────────────────
  // Layout: header(14) | value(18-58) | unit(60) | bar(72-96) | hint(118)
  uint16_t valColor = hasData ? (themeDark ? COLOR_ARC_YEL : 0x000F) : T_DIM();
  tft.setTextSize(4);
  tft.setTextColor(valColor, T_BG());
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
    tft.setTextColor(T_DIM(), T_BG());
    tft.getTextBounds(unit, 0, 0, &tx, &ty, &tw, &th);
    tft.setCursor((SCREEN_W - tw) / 2, 62);
    tft.print(unit);
  }

  // ── Horizontal bar — below value ─────────────────────────────
  const int BAR_X = 6;
  const int BAR_Y = 74;
  const int BAR_W = SCREEN_W - 12;
  const int BAR_H = 22;

  tft.fillRect(BAR_X, BAR_Y, BAR_W, BAR_H, themeDark ? 0x1082 : 0xD69A);
  tft.drawRect(BAR_X - 1, BAR_Y - 1, BAR_W + 2, BAR_H + 2, T_SEP());

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

  { HintSlot s[] = {{"joy","Nav"},{"tri","Menu"}}; drawPageHints(s, 2); }
}

// ─────────────────────────────────────────────────────────────
// showPIDPage — uses dial for numeric, text for boolean
// ─────────────────────────────────────────────────────────────

void ui_gauge(int pidIndex) {
  // No PIDs active — show placeholder instead of blank/crash
  if (getActivePIDCount() == 0) {
    tft.fillScreen(T_BG());
    tft.fillRect(0, 0, SCREEN_W, 14, T_BAR());
    tft.setTextSize(1);
    tft.setTextColor(T_BAR_TXT(), T_BAR());
    tft.setCursor(2, 3); tft.print("GAUGE");
    tft.fillCircle(5, 7, 3, isBTConnected() ? 0x07E0 : 0xF800);
    tft.drawFastHLine(4, 14, SCREEN_W - 8, T_SEP());
    drawCentered("No PIDs selected", 50, 1, T_DIM());
    drawCentered("Open Menu to", 65, 1, T_DIM());
    drawCentered("configure PIDs", 75, 1, T_DIM());
    tft.setTextSize(1); tft.setTextColor(T_DIM(), T_BG());
    drawSquareIcon(SCREEN_W/2 - 14, SCREEN_H - 6, T_DIM());
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
    tft.fillScreen(T_BG());
    if (regenActive && (millis() / 400) % 2 == 0)
      tft.drawRect(0, 0, SCREEN_W, SCREEN_H, COLOR_ARC_RED);
    tft.setTextSize(1);
    tft.setTextColor(T_FG(), T_BG());
    int16_t tx, ty; uint16_t tw, th;
    tft.getTextBounds(title.c_str(), 0, 0, &tx, &ty, &tw, &th);
    tft.setCursor((SCREEN_W - tw) / 2, 3);
    tft.print(title.c_str());
    tft.setTextColor(T_DIM(), T_BG());
    String pageStr = String(aPos) + "/" + String(aTotal);
    tft.setCursor(SCREEN_W - (int)(pageStr.length() * 6) - 2, 3);
    tft.print(pageStr);
    const char* boolStr = !hasData ? "---" : (val != 0.0 ? "ON" : "OFF");
    uint16_t boolColor  = !hasData ? COLOR_DIMGRAY : (val != 0.0 ? COLOR_ARC_RED : 0x07E0);
    drawCentered(boolStr, 50, 3, boolColor);
    { HintSlot s[] = {{"joy","Nav"},{"tri","Menu"}}; drawPageHints(s, 2); }
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
  tft.fillScreen(T_BG());

  // Title bar
  tft.fillRect(0, 0, SCREEN_W, 14, T_BAR());
  tft.drawFastHLine(0, 14, SCREEN_W, T_SEP());
  tft.setTextColor(T_FG());
  tft.setTextSize(1);
  tft.setCursor(2, 3);
  tft.print("SELECT PIDS");

  // Status: active PID count (bg probe removed)
  tft.setTextColor(T_DIM(), T_BAR());
  tft.setCursor(74, 3);
  tft.print(String(getActivePIDCount()) + " active");

  // List — 7 rows
  const int MAX_VISIBLE = 6;
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

    uint16_t bg = isCursor ? T_SEL() : T_BG();

    if (isCursor) tft.fillRect(0, ROW_START + i * ROW_H, SCREEN_W, ROW_H, T_SEL());

    // Checkbox [x]/[ ] — 12px wide
    tft.setTextSize(1);
    tft.setTextColor(active ? TFT_GREEN : T_DIM(), bg);
    tft.setCursor(2, ROW_START + i * ROW_H + 2);
    tft.print(active ? "[x]" : "[ ]");

    // PID name — yellow on cursor, white otherwise
    uint16_t nameColor = isCursor ? TFT_YELLOW : T_FG();

    tft.setTextColor(nameColor, bg);
    tft.setCursor(22, ROW_START + i * ROW_H + 2);
    String label = String(PIDS[idx].name);
    if (label.length() > 13) label = label.substring(0, 13);
    tft.print(label);

    // Status on far right — always show live value if we have it, --- if not
    String status = "";
    uint16_t statusColor = T_DIM();

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
      statusColor = T_DIM();
    }

    // Right-align status at x=127
    int statusX = 127 - (status.length() * 6);
    tft.setTextColor(statusColor, bg);
    tft.setCursor(statusX, ROW_START + i * ROW_H + 2);
    tft.print(status);
  }

  // Hint bar
  { HintSlot s[] = {{"joy","Nav"},{"tri","Back"}}; drawPageHints(s, 2); }

  flushFramebuf();
}

// ─────────────────────────────────────────────────────────────
// PID scan progress — called from obd.cpp probe loop
// ─────────────────────────────────────────────────────────────

void ui_menuPIDProgress(int done, int total, const char* name) {
  tft.fillScreen(T_BG());

  drawTitle("Scanning PIDs");

  // PID name being probed
  tft.setTextSize(1);
  tft.setTextColor(TFT_CYAN, T_BG());
  if (total > 0 && done < total) {
    drawCentered(name, 35, 1, TFT_CYAN);
  } else {
    drawCentered("Done!", 35, 1, TFT_GREEN);
  }

  // Progress bar
  int pct = (total > 0) ? (done * 100 / total) : 100;
  tft.drawRect(4, 55, 120, 10, T_SEP());
  int filled = (int)(118.0f * pct / 100.0f);
  if (filled > 0) tft.fillRect(5, 56, filled, 8, TFT_CYAN);

  // Counter
  String counter = String(done) + " / " + String(total);
  drawCentered(counter.c_str(), 72, 1, T_DIM());

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
  tft.fillScreen(T_BG());

  // Title bar
  tft.fillRect(0, 0, SCREEN_W, 14, T_BAR());
  tft.drawFastHLine(0, 14, SCREEN_W, T_SEP());
  tft.setTextSize(1);
  tft.setTextColor(T_BAR_TXT(), T_BAR());
  tft.setCursor(2, 3);
  tft.print("MENU");

  // Connection status top-right
  tft.setTextColor(connected ? TFT_GREEN : TFT_RED, T_BAR());
  tft.setCursor(SCREEN_W - 42, 3);
  tft.print(connected ? "ONLINE" : "OFFLN");

  // Menu items
  const char* items[] = { "PIDs", "Connect", "Settings" };
  const int   ITEM_COUNT = 3;
  const int   ROW_H   = 20;
  const int   ROW_Y   = 20;

  for (int i = 0; i < ITEM_COUNT; i++) {
    bool sel = (i == selection);
    if (sel) tft.fillRect(0, ROW_Y + i * ROW_H, SCREEN_W, ROW_H, T_SEL());
    tft.setTextSize(1);
    tft.setTextColor(sel ? TFT_YELLOW : T_FG(), sel ? T_SEL() : T_BG());
    tft.setCursor(10, ROW_Y + i * ROW_H + 4);
    tft.print(sel ? "> " : "  ");
    tft.print(items[i]);
  }

  { HintSlot s[] = {{"joy","Nav"},{"tri","Back"}}; drawPageHints(s, 2); }

  flushFramebuf();
}

// ─────────────────────────────────────────────────────────────
// ui_menuSettings — stub, shows placeholder
// ─────────────────────────────────────────────────────────────
void ui_menuSettings(int cursor) {
  tft.fillScreen(T_BG());
  tft.fillRect(0, 0, SCREEN_W, 14, T_BAR());
  tft.setTextSize(1);
  tft.setTextColor(T_BAR_TXT(), T_BAR());
  tft.setCursor(2, 3); tft.print("SETTINGS");

  // Single setting: Theme
  const int ROW_Y = 24, ROW_H = 18;
  bool sel = (cursor == 0);
  if (sel) tft.fillRect(0, ROW_Y, SCREEN_W, ROW_H, T_SEL());

  tft.setTextSize(1);
  tft.setTextColor(sel ? TFT_YELLOW : T_FG(), sel ? T_SEL() : T_BG());
  tft.setCursor(6, ROW_Y + 5);
  tft.print(sel ? "> " : "  ");
  tft.print("Theme");

  // Show current value right-aligned
  const char* themeLabel = themeDark ? "Dark" : "Light";
  tft.setTextColor(TFT_CYAN, sel ? T_SEL() : T_BG());
  tft.setCursor(SCREEN_W - (int)(strlen(themeLabel)*6) - 6, ROW_Y + 5);
  tft.print(themeLabel);

  { HintSlot s[] = {{"joy","Nav"},{"tri","Back"}}; drawPageHints(s, 2); }
  flushFramebuf();
}

