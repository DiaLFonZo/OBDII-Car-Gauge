#include "input.h"
#include "display.h"
#include "ble.h"
#include "obd.h"
#include "pids.h"

#define PRG_BTN        0
#define LONG_PRESS_MS  800

extern AppState appState;

int  gaugePage   = 0;
bool gaugeLocked = false;

static unsigned long lastPageSwitch  = 0;
static bool          dataSeenBefore  = false;
const  unsigned long PAGE_AUTO_MS    = 5000;

void initInput() {
  pinMode(PRG_BTN, INPUT_PULLUP);
}

void resetGaugePage() {
  gaugePage       = 0;
  gaugeLocked     = false;
  dataSeenBefore  = false;   // timer won't start until first data arrives
  lastPageSwitch  = 0;
}

void handleButton() {
  static bool          lastState   = HIGH;
  static unsigned long pressStart  = 0;
  static bool          longHandled = false;

  bool state = digitalRead(PRG_BTN);

  // Press down
  if (state == LOW && lastState == HIGH) {
    pressStart  = millis();
    longHandled = false;
  }

  // Long press (0.8s)
  if (state == LOW && !longHandled && millis() - pressStart > LONG_PRESS_MS) {
    longHandled = true;

    if (appState == STATE_SCANNING) {
      if (isForgetSlot(getSelectedIndex())) {
        forgetSavedDevice();
        setSelectedIndex(0);
      } else {
        showConnecting();
        connectToSelectedDevice(appState);
      }

    } else if (appState == STATE_AUTO_CONNECT) {
      // Skip auto-connect → go to manual scan
      appState = STATE_SCANNING;

    } else if (appState == STATE_GAUGE) {
      // Toggle lock
      if (gaugeLocked) {
        gaugeLocked    = false;
        lastPageSwitch = millis();
      } else {
        gaugeLocked = true;
      }
    }
  }

  // Short press (on release)
  if (state == HIGH && lastState == LOW && !longHandled) {

    if (appState == STATE_SCANNING) {
      int next = (getSelectedIndex() + 1) % getVirtualCount();
      setSelectedIndex(next);

    } else if (appState == STATE_GAUGE) {
      gaugePage      = (gaugePage + 1) % PID_COUNT;
      lastPageSwitch = millis();
    }
  }

  lastState = state;
}

void handlePageAdvance() {
  if (appState != STATE_GAUGE) return;
  if (gaugeLocked) return;

  OBDValues& v = getOBDValues();

  // Wait until first data arrives, then start the 5s timer from that moment
  bool anyData = false;
  for (int i = 0; i < PID_COUNT; i++) {
    if (v.hasData[i]) { anyData = true; break; }
  }
  if (!anyData) return;

  // First time we see data — start the timer now
  if (!dataSeenBefore) {
    dataSeenBefore = true;
    lastPageSwitch = millis();
    return;
  }

  if (millis() - lastPageSwitch >= PAGE_AUTO_MS) {
    gaugePage      = (gaugePage + 1) % PID_COUNT;
    lastPageSwitch = millis();
  }
}
