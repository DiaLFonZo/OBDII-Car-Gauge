#include "ui.h"
#include "app_state.h"
#include "bt.h"
#include "obd.h"
#include "input.h"
#include <Preferences.h>

AppState appState = STATE_GAUGE;

// Silent background connect — only on boot, only if saved device exists
static bool bgConnectDone = false;

void setup() {
  Serial.begin(115200);
  VextON();
  delay(100);
  ui_init();
  initInput();
  buildPIDList();
  loadActivePIDs();

  // Ensure at least the first PID is always active
  // so the gauge always has something to show
  if (getActivePIDCount() == 0) {
    setPIDActive(0, true);
    saveActivePIDs();
  }

  initBT();
  resetGaugePage();
}

void loop() {
  handleButton();

  // ── SILENT BOOT CONNECT ───────────────────────────────────────
  // One-shot: if a saved device exists, try to connect silently
  // while the gauge is already showing. No UI change.
  if (!bgConnectDone) {
    bgConnectDone = true;
    Preferences prefs; prefs.begin("obd", true);
    String savedMac = prefs.getString("mac", ""); prefs.end();
    if (savedMac != "") {
      tryAutoConnect(appState);  // blocks ~1-2s, happens once
    }
    return;
  }

  // ── ELM INIT ─────────────────────────────────────────────────
  if (appState == STATE_INIT_ELM) {
    static unsigned long initStart = 0;
    if (initStart == 0) initStart = millis();
    handleOBD(appState);
    if (appState == STATE_GAUGE) { initStart = 0; return; }
    if (millis() - initStart > 15000) {
      initStart = 0;
      resetOBD();
      appState = STATE_GAUGE;  // give up, user can retry via Menu→Connect
    }
    return;
  }

  // ── GAUGE ─────────────────────────────────────────────────────
  if (appState == STATE_GAUGE) {
    if (isBTConnected()) {
      handleBLE(appState);
      handleOBD(appState);
      if (appState != STATE_GAUGE) return;  // BT dropped → stays in gauge, dot goes red
    }
    ui_gauge(gaugePage);
    return;
  }

  // ── MENU ──────────────────────────────────────────────────────
  if (appState == STATE_MENU) {
    ui_menu(menuCursor, isBTConnected());
    return;
  }

  // ── MENU: PIDs ────────────────────────────────────────────────
  if (appState == STATE_MENU_PIDS) {
    if (isBTConnected()) { handleBLE(appState); handleOBD(appState); }
    static unsigned long lastRedraw = 0;
    if (millis() - lastRedraw > 500) {
      lastRedraw = millis();
      ui_menuPIDs(pidSelectorCursor);
    }
    return;
  }

  // ── MENU: CONNECT ─────────────────────────────────────────────
  if (appState == STATE_MENU_CONNECT) {
    static unsigned long scanDoneAt  = 0;
    static bool          scanStarted = false;
    static bool          screenDrawn = false;
    static AppState      prevState   = STATE_GAUGE;

    bool freshEntry = (prevState != STATE_MENU_CONNECT);
    prevState = appState;

    if (freshEntry) {
      scanStarted = false;
      screenDrawn = false;
    }

    // Iteration 1: draw "Scanning..." screen first, return
    // Iteration 2: actually start scan (blocks ~5s)
    // This way the user always sees the screen before the freeze
    if (!screenDrawn) {
      screenDrawn = true;
      ui_menuConnect(0, 0);   // draw scanning screen
      return;                 // give display time to render
    }

    if (!scanStarted) {
      scanStarted = true;
      startScan();
      scanDoneAt = millis();
    }

    if (getDeviceCount() == 0 && millis() - scanDoneAt > 8000) {
      startScan();
      scanDoneAt = millis();
    }

    ui_menuConnect(getDeviceCount(), getSelectedIndex());
    return;
  }

  // ── MENU: SETTINGS (stub) ─────────────────────────────────────
  if (appState == STATE_MENU_SETTINGS) {
    ui_menuSettings(0);
    return;
  }

  // ── MENU: DEFAULTS (stub) ─────────────────────────────────────
  if (appState == STATE_MENU_DEFAULTS) {
    ui_menuDefaults(0);
    return;
  }
}
