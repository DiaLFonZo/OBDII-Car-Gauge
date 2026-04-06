#include "display.h"
#include "HT_SSD1306Wire.h"
#include "ble.h"
#include "obd.h"
#include "pids.h"
#include <Preferences.h>

static SSD1306Wire display(0x3c, 500000, SDA_OLED, SCL_OLED, GEOMETRY_128_64, RST_OLED);

void initDisplay() {
  display.init();
  display.setFont(ArialMT_Plain_10);
}

#ifndef Vext
#define Vext 36
#endif

void VextON() {
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW);
}

// ─────────────────────────────
// Boot
// ─────────────────────────────

void showBoot() {
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.setFont(ArialMT_Plain_16);
  display.drawString(64, 10, "OBD2 Gauge");
  display.setFont(ArialMT_Plain_10);
  display.drawString(64, 34, "Starting...");
  display.display();
}

// ─────────────────────────────
// Auto-connect
// ─────────────────────────────

void showAutoConnect() {
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  String name = getSavedDeviceName();
  display.setFont(ArialMT_Plain_16);
  display.drawString(64, 8, "Connecting...");
  display.drawHorizontalLine(0, 28, 128);
  display.setFont(ArialMT_Plain_10);
  if (name != "") display.drawString(64, 32, name);
  display.drawString(64, 50, "Hold btn to skip");
  display.display();
}

// ─────────────────────────────
// Connecting
// ─────────────────────────────

void showConnecting() {
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.setFont(ArialMT_Plain_16);
  display.drawString(64, 24, "Connecting...");
  display.display();
}

// ─────────────────────────────
// Waiting for first data
// Shows which PIDs have responded — confirms ELM is alive
// ─────────────────────────────

void showWaitingData() {
  display.clear();

  // Spinner
  static int           frame   = 0;
  static unsigned long lastFrame = 0;
  if (millis() - lastFrame > 300) { frame = (frame + 1) % 4; lastFrame = millis(); }
  const char* spinner[] = { "|", "/", "-", "\\" };

  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.setFont(ArialMT_Plain_16);
  display.drawString(64, 8, "Connected");
  display.drawHorizontalLine(0, 28, 128);
  display.setFont(ArialMT_Plain_10);
  display.drawString(64, 34, "Getting data...");
  display.drawString(64, 50, spinner[frame]);

  display.display();
}

// ─────────────────────────────
// Device list
// ─────────────────────────────

int getVirtualCount() {
  int n = getDeviceCount();
  if (getSavedDeviceName() != "") n++;
  return n;
}

bool isForgetSlot(int index) {
  return (getSavedDeviceName() != "" && index == getDeviceCount());
}

void showScanning(int /*unused*/) {
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.setFont(ArialMT_Plain_16);
  display.drawString(64, 0, "OBD2 Scanner");
  display.drawHorizontalLine(0, 18, 128);
  display.setFont(ArialMT_Plain_10);

  int vCount   = getVirtualCount();
  int selected = getSelectedIndex();

  if (vCount == 0) {
    display.drawString(64, 32, "Scanning BLE...");
    display.display();
    return;
  }

  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString(0, 21, "Select:");
  display.setTextAlignment(TEXT_ALIGN_RIGHT);
  display.drawString(128, 21, String(selected + 1) + "/" + String(vCount));
  display.setTextAlignment(TEXT_ALIGN_LEFT);

  int start = selected - 1;
  if (start < 0) start = 0;
  if (start > vCount - 2) start = max(0, vCount - 2);

  for (int i = 0; i < 2; i++) {
    int index = start + i;
    if (index >= vCount) break;
    String label;
    if (isForgetSlot(index)) {
      label = "[ FORGET SAVED ]";
    } else {
      BLEDeviceEntry* dev = getDevice(index);
      if (!dev) continue;
      label = dev->name != "" ? dev->name : dev->address;
      if (label.length() > 17) label = label.substring(0, 17);
    }
    display.drawString(0, 31 + i * 11, (index == selected ? "> " : "  ") + label);
  }

  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 53, isForgetSlot(selected) ? "Hold to confirm forget"
                                                     : "Hold to connect");
  display.display();
}



// ─────────────────────────────
// PID page
// ─────────────────────────────

void showPIDPage(int pidIndex, bool locked) {
  if (pidIndex < 0 || pidIndex >= PID_COUNT) return;

  const PIDDef& p = PIDS[pidIndex];
  OBDValues&    v = getOBDValues();

  display.clear();

  // Regen flash border
  bool regenActive = false;
  for (int i = 0; i < PID_COUNT; i++) {
    if (PIDS[i].regenFlag && v.hasData[i] && v.values[i] != 0.0) {
      regenActive = true; break;
    }
  }
  if (regenActive && (millis() / 500) % 2 == 0) display.drawRect(0, 0, 128, 64);

  // Title
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString(0, 0, regenActive && !PIDS[pidIndex].regenFlag
                           ? String("! ") + p.name : p.name);

  // Page counter — * prefix when locked
  display.setTextAlignment(TEXT_ALIGN_RIGHT);
  String pageStr = locked
                   ? ("* " + String(pidIndex+1) + "/" + String(PID_COUNT))
                   : (String(pidIndex+1) + "/" + String(PID_COUNT));
  display.drawString(128, 0, pageStr);

  display.drawHorizontalLine(0, 11, 128);

  float val     = v.values[pidIndex];
  bool  hasData = v.hasData[pidIndex];
  display.setTextAlignment(TEXT_ALIGN_CENTER);

  if (p.isBoolean) {
    display.setFont(ArialMT_Plain_24);
    display.drawString(64, 17, !hasData ? "---" : (val != 0.0 ? "ON" : "OFF"));
  } else {
    display.setFont(ArialMT_Plain_24);
    String valStr;
    if      (!hasData)      valStr = "---";
    else if (pidIndex == 0) valStr = String((int)val);
    else                    valStr = String(val, 1);
    display.drawString(64, 15, valStr);

    display.setFont(ArialMT_Plain_10);
    if (strlen(p.unit) > 0) display.drawString(64, 41, p.unit);

    if (hasData) {
      float clamped = constrain(val, p.valMin, p.valMax);
      int   pct     = (int)(100.0 * (clamped - p.valMin) / (p.valMax - p.valMin));
      display.drawProgressBar(4, 53, 120, 8, pct);
    }
  }

  display.display();
}

// ─────────────────────────────
// Dispatcher
// ─────────────────────────────

extern int  gaugePage;
extern bool gaugeLocked;

void updateDisplay(int state) {
  if (state == STATE_SCANNING)     { showScanning(getDeviceCount()); return; }
  if (state == STATE_AUTO_CONNECT) { showAutoConnect();              return; }
  if (state == STATE_CONNECTING)   { showConnecting();               return; }
  if (state == STATE_INIT_ELM)     { showConnecting();               return; }
  if (state == STATE_GAUGE) {
    OBDValues& v = getOBDValues();
    bool anyData = false;
    for (int i = 0; i < PID_COUNT; i++) { if (v.hasData[i]) { anyData = true; break; } }
    if (!anyData) { showWaitingData(); return; }
    showPIDPage(gaugePage, gaugeLocked);
  }
}
