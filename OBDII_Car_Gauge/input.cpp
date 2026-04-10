#include "input.h"
#include "ui.h"
#include "bt.h"
#include "obd.h"
#include "pids.h"
#include <Wire.h>
#include <Preferences.h>

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
#define MENU_ITEM_DEFAULTS 3
#define MENU_ITEM_COUNT    4

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
  if (appState == STATE_MENU_PIDS && (heldBit & (BIT_JU|BIT_JD))) {
    static unsigned long lastScroll = 0;
    unsigned long interval = (millis()-pressStart > 600) ? 80 : 300;
    if (millis() - lastScroll > interval) {
      lastScroll = millis();
      if (heldBit & BIT_JU) pidSelectorCursor = max(0, pidSelectorCursor-1);
      if (heldBit & BIT_JD) pidSelectorCursor = min(PID_COUNT-1, pidSelectorCursor+1);
      ui_menuPIDs(pidSelectorCursor);
    }
  }

  // Continuous scroll — connect list
  if (appState == STATE_MENU_CONNECT && (heldBit & (BIT_JU|BIT_JD))) {
    static unsigned long lastScroll2 = 0;
    unsigned long interval = (millis()-pressStart > 600) ? 80 : 300;
    if (millis() - lastScroll2 > interval) {
      lastScroll2 = millis();
      int total = getVirtualCount();
      if (total > 0) {
        if (heldBit & BIT_JU) setSelectedIndex(max(0, getSelectedIndex()-1));
        if (heldBit & BIT_JD) setSelectedIndex(min(total-1, getSelectedIndex()+1));
      }
    }
  }

  if (justPressed && heldBit == 0) {
    heldBit = justPressed; pressStart = millis(); longHandled = false;
  }

  // Long press
  if (heldBit && !longHandled && millis()-pressStart > LONG_PRESS_MS) {
    longHandled = true;
    if (appState == STATE_MENU_CONNECT && (heldBit & BIT_JP)) {
      int sel = getSelectedIndex();
      if (isSavedSlot(sel)) { forgetSavedDevice(); setSelectedIndex(0); ui_resetScanDisplay(); }
      else { setSelectedIndex(virtualToReal(sel)); connectToSelectedDevice(appState); }
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
        if (heldBit & BIT_JU) menuCursor = max(0, menuCursor-1);
        if (heldBit & BIT_JD) menuCursor = min(MENU_ITEM_COUNT-1, menuCursor+1);
        if (heldBit & BIT_JP || heldBit & BIT_JR) {
          if      (menuCursor==MENU_ITEM_PIDS)     { pidSelectorCursor=0; appState=STATE_MENU_PIDS; }
          else if (menuCursor==MENU_ITEM_CONNECT)  { if(isBTConnected()){resetOBD();disconnectBT();ui_leds_off();} ui_resetScanDisplay(); resumeScan(); appState=STATE_MENU_CONNECT; }
          else if (menuCursor==MENU_ITEM_SETTINGS) { appState=STATE_MENU_SETTINGS; }
          else if (menuCursor==MENU_ITEM_DEFAULTS) { appState=STATE_MENU_DEFAULTS; }
        }
        if (heldBit & BIT_BTNB || heldBit & BIT_JL) appState=STATE_GAUGE;
      }

      else if (appState == STATE_MENU_PIDS) {
        if (heldBit & BIT_JP) { setPIDActive(pidSelectorCursor,!isPIDActive(pidSelectorCursor)); ui_menuPIDs(pidSelectorCursor); }
        if (heldBit & BIT_BTNB) { saveActivePIDs(); resetPollGroups(); resetGaugePage(); appState=STATE_MENU; }
        if (heldBit & BIT_BTNA) { saveActivePIDs(); resetPollGroups(); resetGaugePage(); appState=STATE_GAUGE; }
      }

      else if (appState == STATE_MENU_CONNECT) {
        if (heldBit & BIT_JU) { int n=getSelectedIndex()-1; if(n<0)n=getVirtualCount()-1; setSelectedIndex(n); }
        if (heldBit & BIT_JD) { setSelectedIndex((getSelectedIndex()+1)%max(1,getVirtualCount())); }
        if (heldBit & BIT_JP) {
          int sel=getSelectedIndex();
          if (isSavedSlot(sel)) {
            Preferences prefs; prefs.begin("obd",true); String mac=prefs.getString("mac",""); prefs.end();
            bool found=false;
            for (int i=0;i<getDeviceCount();i++) {
              BTDeviceEntry* dev=getDevice(i);
              if (dev && dev->address==mac) { setSelectedIndex(i); connectToSelectedDevice(appState); found=true; break; }
            }
            if (!found) appState=STATE_AUTO_CONNECT;
          } else { setSelectedIndex(virtualToReal(sel)); connectToSelectedDevice(appState); }
        }
        if (heldBit & BIT_BTNB || heldBit & BIT_JL) appState=STATE_MENU;
      }

      else if (appState==STATE_MENU_SETTINGS || appState==STATE_MENU_DEFAULTS) {
        if (heldBit & BIT_BTNB || heldBit & BIT_JL) appState=STATE_MENU;
      }

    }
    heldBit = 0;
  }

  prevP0 = p0;
}

void handlePageAdvance() {}
