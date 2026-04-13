#include "nav.h"
#include "ui.h"
#include "bt.h"
#include "obd.h"
#include "pids.h"

// ─────────────────────────────────────────────────────────────
// Menu item indices — must match ui_menu() item order
// ─────────────────────────────────────────────────────────────
#define MENU_ITEM_PIDS     0
#define MENU_ITEM_CONNECT  1
#define MENU_ITEM_SETTINGS 2
#define MENU_ITEM_COUNT    3

// ─────────────────────────────────────────────────────────────
// Connect page virtual list helpers
// index 0           = [ SCAN ]
// index 1..saved    = saved devices
// index saved+1..   = new scan results
// ─────────────────────────────────────────────────────────────
static int connectTotalItems() {
  int saved = getSavedDeviceCount();
  int scan  = getDeviceCount();
  int newCount = 0;
  for (int i = 0; i < scan; i++) {
    BTDeviceEntry* se = getDevice(i); if (!se) continue;
    bool found = false;
    for (int j = 0; j < saved; j++) {
      BTSavedDevice* sd = getSavedDevice(j);
      if (sd && sd->mac == se->address) { found = true; break; }
    }
    if (!found) newCount++;
  }
  return 1 + saved + newCount;
}

// ─────────────────────────────────────────────────────────────
// handleNav — maps intents to state transitions
// ─────────────────────────────────────────────────────────────
void handleNav(InputIntent intent, AppState &state) {
  if (intent == INTENT_NONE) return;

  // ── GAUGE ───────────────────────────────────────────────────
  if (state == STATE_GAUGE) {
    switch (intent) {
      case INTENT_RIGHT:
        for (int i = 1; i <= PID_COUNT; i++) {
          int n = (gaugePage + i) % PID_COUNT;
          if (isPIDActive(n)) { gaugePage = n; break; }
        }
        break;
      case INTENT_LEFT:
        for (int i = 1; i <= PID_COUNT; i++) {
          int n = (gaugePage - i + PID_COUNT) % PID_COUNT;
          if (isPIDActive(n)) { gaugePage = n; break; }
        }
        break;
      case INTENT_BACK:
      case INTENT_MENU:
        // Triangle or dedicated menu button opens menu from gauge
        menuCursor = 0;
        state = STATE_MENU;
        break;
      default: break;
    }
    return;
  }

  // ── MENU ────────────────────────────────────────────────────
  if (state == STATE_MENU) {
    switch (intent) {
      case INTENT_UP:
        menuCursor = (menuCursor <= 0) ? MENU_ITEM_COUNT-1 : menuCursor-1;
        break;
      case INTENT_DOWN:
        menuCursor = (menuCursor >= MENU_ITEM_COUNT-1) ? 0 : menuCursor+1;
        break;
      case INTENT_SELECT:
      case INTENT_RIGHT:
        if (menuCursor == MENU_ITEM_PIDS) {
          pidSelectorCursor = 0;
          state = STATE_MENU_PIDS;
        } else if (menuCursor == MENU_ITEM_CONNECT) {
          if (isBTConnected()) { resetOBD(); disconnectBT(); ui_leds_off(); }
          state = STATE_MENU_CONNECT;
        } else if (menuCursor == MENU_ITEM_SETTINGS) {
          menuCursor = 0;
          state = STATE_MENU_SETTINGS;
        }
        break;
      case INTENT_BACK:
      case INTENT_LEFT:
        state = STATE_GAUGE;
        break;
      default: break;
    }
    return;
  }

  // ── MENU: PIDs ──────────────────────────────────────────────
  // Virtual list: index 0 = Toggle All, index 1..PID_COUNT = PIDs
  if (state == STATE_MENU_PIDS) {
    const int TOTAL = PID_COUNT + 1;
    switch (intent) {
      case INTENT_UP:
        pidSelectorCursor = (pidSelectorCursor <= 0) ? TOTAL-1 : pidSelectorCursor-1;
        ui_menuPIDs(pidSelectorCursor);
        break;
      case INTENT_DOWN:
        pidSelectorCursor = (pidSelectorCursor >= TOTAL-1) ? 0 : pidSelectorCursor+1;
        ui_menuPIDs(pidSelectorCursor);
        break;
      case INTENT_SELECT:
        if (pidSelectorCursor == 0) {
          // Toggle All / None
          bool anyActive = false;
          for (int i = 0; i < PID_COUNT; i++) if (isPIDActive(i)) { anyActive = true; break; }
          for (int i = 0; i < PID_COUNT; i++) setPIDActive(i, !anyActive);
          saveActivePIDs(); resetPollGroups(); resetGaugePage();
        } else {
          setPIDActive(pidSelectorCursor - 1, !isPIDActive(pidSelectorCursor - 1));
        }
        ui_menuPIDs(pidSelectorCursor);
        break;
      case INTENT_BACK:
        saveActivePIDs(); resetPollGroups(); resetGaugePage();
        state = STATE_MENU;
        break;
      case INTENT_LONG_SELECT:
        saveActivePIDs(); resetPollGroups(); resetGaugePage();
        state = STATE_GAUGE;
        break;
      default: break;
    }
    return;
  }

  // ── MENU: CONNECT ───────────────────────────────────────────
  if (state == STATE_MENU_CONNECT) {
    int total = connectTotalItems();
    int saved = getSavedDeviceCount();
    int scan  = getDeviceCount();

    // Build new scan result map
    int newScanMap[MAX_SCAN_DEVICES] = {};
    int newScanCount = 0;
    for (int i = 0; i < scan; i++) {
      BTDeviceEntry* se = getDevice(i); if (!se) continue;
      bool found = false;
      for (int j = 0; j < saved; j++) {
        BTSavedDevice* sd = getSavedDevice(j);
        if (sd && sd->mac == se->address) { found = true; break; }
      }
      if (!found) newScanMap[newScanCount++] = i;
    }

    switch (intent) {
      case INTENT_UP:
        setSelectedIndex(getSelectedIndex() <= 0 ? total-1 : getSelectedIndex()-1);
        break;
      case INTENT_DOWN:
        setSelectedIndex((getSelectedIndex() + 1) % total);
        break;
      case INTENT_SELECT: {
        int sel = getSelectedIndex();
        if (sel == 0) {
          startScan();
        } else if (sel <= saved) {
          int di = sel - 1;
          setDefaultDevice(di);
          BTSavedDevice* dev = getSavedDevice(di);
          if (dev) startConnectAsync(dev->mac, dev->name);
        } else {
          int ni = sel - 1 - saved;
          if (ni >= 0 && ni < newScanCount) {
            BTDeviceEntry* dev = getDevice(newScanMap[ni]);
            if (dev) startConnectAsync(dev->address, dev->name);
          }
        }
        break;
      }
      case INTENT_LONG_SELECT: {
        // Long select on saved device = forget
        int sel = getSelectedIndex();
        if (sel >= 1 && sel <= saved) {
          forgetDevice(sel - 1);
          setSelectedIndex(0);
        }
        break;
      }
      case INTENT_BACK:
      case INTENT_LEFT:
        state = STATE_MENU;
        break;
      default: break;
    }
    return;
  }

  // ── MENU: SETTINGS ──────────────────────────────────────────
  if (state == STATE_MENU_SETTINGS) {
    switch (intent) {
      case INTENT_UP:
        menuCursor = max(0, menuCursor - 1);
        break;
      case INTENT_DOWN:
        menuCursor = min(0, menuCursor + 1);  // max=0 until more settings added
        break;
      case INTENT_SELECT:
        if (menuCursor == 0) ui_setTheme(!ui_isDarkTheme());
        break;
      case INTENT_BACK:
      case INTENT_LEFT:
        state = STATE_MENU;
        break;
      default: break;
    }
    return;
  }
}
