#include "display.h"
#include "ble.h"
#include "obd.h"
#include "input.h"
#include <Preferences.h>

AppState appState       = STATE_BOOT;
unsigned long bootStartTime = 0;

void setup() {
  Serial.begin(115200);
  VextON();
  delay(100);
  initDisplay();
  initInput();
  initBLE();
  bootStartTime = millis();
}

void loop() {
  handleButton();
  handlePageAdvance();

  // ── BOOT ────────────────────────────────────────────────────
  if (appState == STATE_BOOT) {
    showBoot();
    if (millis() - bootStartTime > 5000) {
      startScan();
      Preferences prefs;
      prefs.begin("obd", true);
      String savedMac = prefs.getString("mac", "");
      prefs.end();
      appState = (savedMac != "") ? STATE_AUTO_CONNECT : STATE_SCANNING;
    }
    return;
  }

  // ── AUTO-CONNECT ─────────────────────────────────────────────
  if (appState == STATE_AUTO_CONNECT) {
    static unsigned long autoStart = 0;
    if (autoStart == 0) autoStart = millis();

    showAutoConnect();

    // Timeout → manual scan
    if (millis() - autoStart > 8000) {
      autoStart = 0;
      appState  = STATE_SCANNING;
      return;
    }

    // Mandatory 2s window — button can skip during this time
    // handleButton() runs at top of loop so a hold sets STATE_SCANNING
    if (millis() - autoStart < 2000) {
      return;
    }

    // 2s elapsed — connect if not skipped
    if (appState == STATE_AUTO_CONNECT) {
      autoStart = 0;
      tryAutoConnect(appState);
    }
    return;
  }

  // ── SCANNING ─────────────────────────────────────────────────
  if (appState == STATE_SCANNING) {
    updateDisplay(appState);
    return;
  }

  // ── NORMAL FLOW ──────────────────────────────────────────────
  handleBLE(appState);
  handleOBD(appState);
  updateDisplay(appState);
}
