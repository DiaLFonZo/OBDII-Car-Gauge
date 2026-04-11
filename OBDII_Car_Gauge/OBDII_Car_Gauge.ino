#include "ui.h"
#include "app_state.h"
#include "bt.h"
#include "obd.h"
#include "input.h"
#include "nav.h"
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
  handleNav(getIntent(), appState);

  // ── SILENT BOOT CONNECT ───────────────────────────────────────
  // One-shot after first gauge draw — use async connect so screen stays live
  if (!bgConnectDone && appState == STATE_GAUGE) {
    bgConnectDone = true;
    if (hasSavedDevice()) {
      BTSavedDevice* dev = getSavedDevice(getDefaultDeviceIndex());
      if (dev) startConnectAsync(dev->mac, dev->name);
    }
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
    // Check if background boot connect just finished
    bool connectOk = false;
    if (isConnectFinished(connectOk) && connectOk) {
      resetOBD(); resetGaugePage();
      appState = STATE_INIT_ELM;
      return;
    }
    if (isBTConnected()) {
      handleBLE(appState);
      handleOBD(appState);
      if (appState != STATE_GAUGE) return;
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
    // Check if async connect just finished
    bool success = false;
    if (isConnectFinished(success)) {
      if (success) {
        resetOBD(); resetGaugePage();
        appState = STATE_INIT_ELM;
        return;
      }
      // Failed — stay on connect page
    }
    ui_menuConnect(0, getSelectedIndex());
    if (isScanRunning())    ui_scanOverlay();     // overlay while scanning
    if (isConnectRunning()) ui_connectOverlay();  // overlay while connecting
    return;
  }

  // ── MENU: SETTINGS (stub) ─────────────────────────────────────
  if (appState == STATE_MENU_SETTINGS) {
    ui_menuSettings(menuCursor);
    return;
  }

}
