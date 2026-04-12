#include "hal.h"
#ifdef TARGET_HELTEC

// ═══════════════════════════════════════════════════════════════
//  ui_heltec.cpp — UI implementation for Heltec WiFi LoRa 32 V3
//
//  Display: SSD1306 OLED 128×64, monochrome, I2C
//  SDA: GPIO17  SCL: GPIO18  RST: GPIO21
//  Library: Adafruit_SSD1306 or U8g2
//
//  No color, no framebuffer — draw directly to OLED.
//  Theme is irrelevant (monochrome), animations are simple.
//  Layout is adapted for 128×64 vs CyberPi's 128×128.
// ═══════════════════════════════════════════════════════════════

#include "ui.h"
#include "app_state.h"
#include "bt.h"
#include "obd.h"
#include "pids.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_W  128
#define SCREEN_H   64
#define OLED_ADDR  0x3C
#define OLED_RST   21

static Adafruit_SSD1306 oled(SCREEN_W, SCREEN_H, &Wire, OLED_RST);

// ── Helpers ──────────────────────────────────────────────────────
static void centered(const char* text, int y, int size = 1) {
  oled.setTextSize(size);
  int w = strlen(text) * 6 * size;
  oled.setCursor((SCREEN_W - w) / 2, y);
  oled.print(text);
}

static void flush() { oled.display(); }

static void drawHintBar(const char* left, const char* right) {
  oled.drawFastHLine(0, 54, SCREEN_W, WHITE);
  oled.setTextSize(1);
  oled.setTextColor(WHITE);
  oled.setCursor(2, 57);    oled.print(left);
  int rw = strlen(right) * 6;
  oled.setCursor(SCREEN_W - rw - 2, 57); oled.print(right);
}

// ── Lifecycle ────────────────────────────────────────────────────
void ui_init() {
  Wire.begin(17, 18);
  oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  oled.clearDisplay();
  oled.setTextColor(WHITE);
  oled.display();
}

// ── Gauge ────────────────────────────────────────────────────────
void ui_gauge(int pidIndex) {
  if (pidIndex < 0 || pidIndex >= PID_COUNT) return;

  const PIDDef& p = PIDS[pidIndex];
  OBDValues&    v = getOBDValues();
  float val     = v.values[pidIndex];
  bool  hasData = v.hasData[pidIndex];

  oled.clearDisplay();
  oled.setTextColor(WHITE);

  // Connection dot top-left
  if (isBTConnected()) oled.fillCircle(3, 3, 2, WHITE);
  else oled.drawCircle(3, 3, 2, WHITE);

  // PID name centered top
  centered(p.name, 0, 1);

  // Value — large
  String valStr = hasData ? String((int)val) : "---";
  oled.setTextSize(3);
  int vw = valStr.length() * 18;
  oled.setCursor((SCREEN_W - vw) / 2, 14);
  oled.print(valStr);

  // Unit
  if (strlen(p.unit) > 0) {
    oled.setTextSize(1);
    centered(p.unit, 42, 1);
  }

  // Bar
  int barW = SCREEN_W - 8;
  oled.drawRect(4, 48, barW, 5, WHITE);
  if (hasData) {
    float pct = (val - p.valMin) / (p.valMax - p.valMin);
    pct = constrain(pct, 0, 1);
    oled.fillRect(5, 49, (int)(pct * (barW - 2)), 3, WHITE);
  }

  drawHintBar("< >:pages", "btn:menu");
  flush();
}

// ── Menu ─────────────────────────────────────────────────────────
void ui_menu(int selection, bool connected) {
  oled.clearDisplay();
  oled.setTextColor(WHITE);
  oled.setTextSize(1);
  oled.setCursor(2, 0); oled.print("MENU");
  oled.setCursor(90, 0); oled.print(connected ? "ON" : "OFF");
  oled.drawFastHLine(0, 9, SCREEN_W, WHITE);

  const char* items[] = { "PIDs", "Connect", "Settings" };
  for (int i = 0; i < 3; i++) {
    bool sel = (i == selection);
    if (sel) { oled.fillRect(0, 11 + i*14, SCREEN_W, 13, WHITE); oled.setTextColor(BLACK); }
    else oled.setTextColor(WHITE);
    oled.setCursor(4, 13 + i*14);
    oled.print(sel ? "> " : "  "); oled.print(items[i]);
  }
  oled.setTextColor(WHITE);
  drawHintBar("btn:cycle", "hold:sel");
  flush();
}

// ── Menu: PIDs ───────────────────────────────────────────────────
void ui_menuPIDs(int cursor) {
  oled.clearDisplay();
  oled.setTextColor(WHITE);
  oled.setTextSize(1);
  oled.setCursor(2, 0); oled.print("SELECT PIDS");
  oled.drawFastHLine(0, 9, SCREEN_W, WHITE);

  const int ROWS = 4;
  int start = max(0, cursor - 1);
  if (start > PID_COUNT - ROWS) start = max(0, PID_COUNT - ROWS);

  for (int i = 0; i < ROWS; i++) {
    int idx = start + i;
    if (idx >= PID_COUNT) break;
    bool sel = (idx == cursor);
    bool act = isPIDActive(idx);
    if (sel) { oled.fillRect(0, 11 + i*12, SCREEN_W, 11, WHITE); oled.setTextColor(BLACK); }
    else oled.setTextColor(WHITE);
    oled.setCursor(2, 13 + i*12);
    oled.print(act ? "[x] " : "[ ] ");
    oled.print(PIDS[idx].name);
    oled.setTextColor(WHITE);
  }
  drawHintBar("btn:next", "hold:tog");
  flush();
}

// ── Menu: Connect ────────────────────────────────────────────────
void ui_menuConnect(int /*unused*/, int selected) {
  int savedCount = getSavedDeviceCount();
  int totalItems = 1 + savedCount;

  oled.clearDisplay();
  oled.setTextColor(WHITE);
  oled.setTextSize(1);
  oled.setCursor(2, 0); oled.print("CONNECT");
  oled.drawFastHLine(0, 9, SCREEN_W, WHITE);

  const int ROWS = 4;
  int start = max(0, selected - 1);
  if (start > totalItems - ROWS) start = max(0, totalItems - ROWS);

  for (int i = 0; i < ROWS; i++) {
    int index = start + i;
    if (index >= totalItems) break;
    bool sel = (index == selected);
    if (sel) { oled.fillRect(0, 11 + i*12, SCREEN_W, 11, WHITE); oled.setTextColor(BLACK); }
    else oled.setTextColor(WHITE);
    oled.setCursor(2, 13 + i*12);
    if (index == 0) {
      oled.print("[ SCAN ]");
    } else {
      BTSavedDevice* dev = getSavedDevice(index - 1);
      bool isDef = ((index - 1) == getDefaultDeviceIndex());
      oled.print(isDef ? "* " : "  ");
      if (dev) oled.print(dev->name != "" ? dev->name : dev->mac);
    }
    oled.setTextColor(WHITE);
  }
  drawHintBar("btn:next", "hold:sel");
  flush();
}

// ── Menu: Settings ───────────────────────────────────────────────
void ui_menuSettings(int cursor) {
  oled.clearDisplay();
  oled.setTextColor(WHITE);
  oled.setTextSize(1);
  oled.setCursor(2, 0); oled.print("SETTINGS");
  oled.drawFastHLine(0, 9, SCREEN_W, WHITE);
  bool sel = (cursor == 0);
  if (sel) { oled.fillRect(0, 11, SCREEN_W, 13, WHITE); oled.setTextColor(BLACK); }
  else oled.setTextColor(WHITE);
  oled.setCursor(4, 14); oled.print("Theme: N/A (mono)");
  oled.setTextColor(WHITE);
  drawHintBar("btn:next", "hold:back");
  flush();
}

// ── Stubs ────────────────────────────────────────────────────────
void ui_menuDefaults(int) {}
void ui_autoConnect() {
  oled.clearDisplay(); oled.setTextColor(WHITE);
  centered("Connecting...", 24, 1); flush();
}
void ui_resetAutoConnect() {}
void ui_connecting() {
  oled.clearDisplay(); oled.setTextColor(WHITE);
  centered("Connecting...", 24, 1); flush();
}
void ui_resetScanDisplay() {}
void ui_leds(float, float) {}
void ui_leds_off() {}
void ui_flush() { flush(); }
void ui_scanOverlay() {
  static int f = 0; f = (f + 1) % 4;
  const char* sp[] = {"-","\\","|","/"};
  oled.fillRect(30, 20, 68, 24, BLACK);
  oled.drawRect(30, 20, 68, 24, WHITE);
  oled.setTextSize(1); oled.setTextColor(WHITE);
  oled.setCursor(36, 26); oled.print("Scanning ");
  oled.print(sp[f]); flush();
}
void ui_connectOverlay() {
  oled.fillRect(20, 20, 88, 24, BLACK);
  oled.drawRect(20, 20, 88, 24, WHITE);
  oled.setTextSize(1); oled.setTextColor(WHITE);
  centered("Connecting...", 28, 1); flush();
}
void ui_setTheme(bool) {}   // monochrome — theme not applicable
bool ui_isDarkTheme() { return true; }
void VextON() {
  pinMode(36, OUTPUT); digitalWrite(36, LOW);
}

#endif // TARGET_HELTEC
