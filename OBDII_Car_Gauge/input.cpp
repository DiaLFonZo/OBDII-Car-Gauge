#include "input.h"
#include "ui.h"
#include "bt.h"
#include "obd.h"
#include "pids.h"
#include <Wire.h>

#define AW9523B_ADDR   0x58
#define AW_P0_IN_REG   0x00
#define AW_P0_CFG_REG  0x04

#define BIT_JL   (1 << 0)
#define BIT_JU   (1 << 1)
#define BIT_JR   (1 << 2)
#define BIT_JP   (1 << 3)
#define BIT_JD   (1 << 4)
#define BIT_BTNB (1 << 5)
#define BIT_BTNA (1 << 6)

#define LONG_PRESS_MS 800

extern AppState appState;

int  gaugePage         = 0;
bool gaugeLocked       = false;
int  pidSelectorCursor = 0;
int  menuCursor        = 0;

#define MENU_ITEM_PIDS     0
#define MENU_ITEM_CONNECT  1
#define MENU_ITEM_SETTINGS 2
#define MENU_ITEM_COUNT    3

static uint8_t readP0() {
  Wire.beginTransmission(AW9523B_ADDR);
  Wire.write(AW_P0_IN_REG);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)AW9523B_ADDR, (uint8_t)1);
  if (Wire.available()) return Wire.read();
  return 0xFF;
}

void initInput() {
  Wire.beginTransmission(AW9523B_ADDR);
  Wire.write(AW_P0_CFG_REG);
  Wire.write(0xFF);
  Wire.endTransmission();
}

void resetGaugePage() {
  gaugePage = 0;
  for (int i = 0; i < PID_COUNT; i++) {
    if (isPIDActive(i)) { gaugePage = i; break; }
  }
}

void handleButton() {
  static uint8_t       prevP0      = 0x7F;
  static unsigned long pressStart  = 0;
  static uint8_t       heldBit     = 0;
  static bool          longHandled = false;

  uint8_t p0 = readP0();
  const uint8_t MASK = BIT_JL|BIT_JU|BIT_JR|BIT_JP|BIT_JD|BIT_BTNB|BIT_BTNA;
  p0   &= MASK;
  uint8_t prev = prevP0 & MASK;

  uint8_t justPressed  = (uint8_t)(~p0) & prev;
  uint8_t justReleased = p0 & (uint8_t)(~prev);

  // Continuous scroll — PID list
  if (appState == STATE_MENU_PIDS && (heldBit & (BIT_JU|BIT_JD)) && millis()-pressStart > 400) {
    static unsigned long lastScroll = 0;
    unsigned long interval = (millis()-pressStart > 800) ? 100 : 300;
    if (millis() - lastScroll > interval) {
      lastScroll = millis();
      if (heldBit & BIT_JU) pidSelectorCursor = (pidSelectorCursor <= 0) ? PID_COUNT-1 : pidSelectorCursor-1;
      if (heldBit & BIT_JD) pidSelectorCursor = (pidSelectorCursor >= PID_COUNT-1) ? 0 : pidSelectorCursor+1;
      ui_menuPIDs(pidSelectorCursor);
    }
  }

  // Continuous scroll — connect list
  if (appState == STATE_MENU_CONNECT && (heldBit & (BIT_JU|BIT_JD)) && millis()-pressStart > 400) {
    static unsigned long lastScroll2 = 0;
    unsigned long interval = (millis()-pressStart > 800) ? 100 : 300;
    if (millis() - lastScroll2 > interval) {
      lastScroll2 = millis();
      // Count new scan results
      int savedCount = getSavedDeviceCount();
      int scanCount  = getDeviceCount();
      int newCount   = 0;
      for (int i = 0; i < scanCount; i++) {
        BTDeviceEntry* se = getDevice(i); if (!se) continue;
        bool saved = false;
        for (int j = 0; j < savedCount; j++) { BTSavedDevice* sd = getSavedDevice(j); if (sd && sd->mac == se->address) { saved=true; break; } }
        if (!saved) newCount++;
      }
      int total = 1 + savedCount + newCount;
      if (total > 0) {
        if (heldBit & BIT_JU) setSelectedIndex(getSelectedIndex() <= 0 ? total-1 : getSelectedIndex()-1);
        if (heldBit & BIT_JD) setSelectedIndex(getSelectedIndex() >= total-1 ? 0 : getSelectedIndex()+1);
      }
    }
  }

  if (justPressed && heldBit == 0) {
    heldBit = justPressed; pressStart = millis(); longHandled = false;
  }

  // Long press
  if (heldBit && !longHandled && millis()-pressStart > LONG_PRESS_MS) {
    longHandled = true;
    if (appState == STATE_MENU_CONNECT && (heldBit & BIT_BTNA)) {
      // Long press Square on a saved device = forget it
      int sel = getSelectedIndex();
      int savedCount = getSavedDeviceCount();
      if (sel >= 1 && sel <= savedCount) {
        forgetDevice(sel - 1);
        setSelectedIndex(0);
      }
    }
    if (appState == STATE_MENU_PIDS && (heldBit & (BIT_JP|BIT_BTNA))) {
      saveActivePIDs(); resetPollGroups(); resetGaugePage(); appState = STATE_GAUGE;
    }
  }

  // Short press
  if (justReleased & heldBit) {
    if (!longHandled) {

      if (appState == STATE_GAUGE) {
        if (heldBit & BIT_JR) {
          for (int i=1; i<=PID_COUNT; i++) { int n=(gaugePage+i)%PID_COUNT; if(isPIDActive(n)){gaugePage=n;break;} }
        }
        if (heldBit & BIT_JL) {
          for (int i=1; i<=PID_COUNT; i++) { int n=(gaugePage-i+PID_COUNT)%PID_COUNT; if(isPIDActive(n)){gaugePage=n;break;} }
        }
        if (heldBit & BIT_BTNA) { menuCursor=0; appState=STATE_MENU; }
        if (heldBit & BIT_BTNB) {
          // Triangle in gauge = back = nothing (already at top level)
          (void)0;
        }
      }

      else if (appState == STATE_MENU) {
        if (heldBit & BIT_JU) menuCursor = (menuCursor <= 0) ? MENU_ITEM_COUNT-1 : menuCursor-1;
        if (heldBit & BIT_JD) menuCursor = (menuCursor >= MENU_ITEM_COUNT-1) ? 0 : menuCursor+1;
        if (heldBit & BIT_JP || heldBit & BIT_JR) {
          if      (menuCursor==MENU_ITEM_PIDS)     { pidSelectorCursor=0; appState=STATE_MENU_PIDS; }
          else if (menuCursor==MENU_ITEM_CONNECT)  { if(isBTConnected()){resetOBD();disconnectBT();ui_leds_off();} appState=STATE_MENU_CONNECT; }
          else if (menuCursor==MENU_ITEM_SETTINGS) { menuCursor=0; appState=STATE_MENU_SETTINGS; }
        }
        if (heldBit & BIT_BTNB || heldBit & BIT_JL) appState=STATE_GAUGE;
      }

      else if (appState == STATE_MENU_PIDS) {
        if (heldBit & BIT_JP) { setPIDActive(pidSelectorCursor,!isPIDActive(pidSelectorCursor)); ui_menuPIDs(pidSelectorCursor); }
        if (heldBit & BIT_BTNB) { saveActivePIDs(); resetPollGroups(); resetGaugePage(); appState=STATE_MENU; }
        if (heldBit & BIT_BTNA) { saveActivePIDs(); resetPollGroups(); resetGaugePage(); appState=STATE_GAUGE; }
      }

      else if (appState == STATE_MENU_CONNECT) {
        int savedCount = getSavedDeviceCount();
        int scanCount  = getDeviceCount();

        // Count new scan results (not already saved)
        int newScanCount = 0;
        int newScanMap[MAX_SCAN_DEVICES] = {};  // virtual→scan index
        for (int i = 0; i < scanCount; i++) {
          BTDeviceEntry* se = getDevice(i);
          if (!se) continue;
          bool saved = false;
          for (int j = 0; j < savedCount; j++) {
            BTSavedDevice* sd = getSavedDevice(j);
            if (sd && sd->mac == se->address) { saved = true; break; }
          }
          if (!saved) newScanMap[newScanCount++] = i;
        }

        int totalItems = 1 + savedCount + newScanCount;

        if (heldBit & BIT_JU) {
          int n = getSelectedIndex() - 1;
          if (n < 0) n = totalItems - 1;
          setSelectedIndex(n);
        }
        if (heldBit & BIT_JD) {
          setSelectedIndex((getSelectedIndex() + 1) % totalItems);
        }
        if (heldBit & BIT_JP) {
          int sel = getSelectedIndex();
          if (sel == 0) {
            // SCAN — starts async on Core 0, loop animates while it runs
            startScan();
          } else if (sel <= savedCount) {
            // Connect to saved device — async
            int di = sel - 1;
            setDefaultDevice(di);
            BTSavedDevice* dev = getSavedDevice(di);
            if (dev) startConnectAsync(dev->mac, dev->name);
          } else {
            // Connect to new scan result — async
            int ni = sel - 1 - savedCount;
            if (ni >= 0 && ni < newScanCount) {
              BTDeviceEntry* dev = getDevice(newScanMap[ni]);
              if (dev) startConnectAsync(dev->address, dev->name);
            }
          }
        }
        if (heldBit & BIT_BTNB || heldBit & BIT_JL) appState = STATE_MENU;
      }

      else if (appState == STATE_MENU_SETTINGS) {
        // Up/Down navigates settings items (only 1 for now, future-proof)
        if (heldBit & BIT_JU) menuCursor = max(0, menuCursor - 1);
        if (heldBit & BIT_JD) menuCursor = min(0, menuCursor + 1); // max item = 0 for now
        if (heldBit & BIT_JP) {
          // Center click = toggle selected setting
          if (menuCursor == 0) ui_setTheme(!ui_isDarkTheme());
        }
        if (heldBit & BIT_BTNB || heldBit & BIT_JL) appState = STATE_MENU;
      }

    }
    heldBit = 0;
  }

  prevP0 = p0;
}

void handlePageAdvance() {}
