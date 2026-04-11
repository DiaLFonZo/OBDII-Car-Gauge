#include "hal.h"
#ifdef TARGET_CYBERPI

#include "input.h"
#include "obd.h"
#include "pids.h"
#include <Wire.h>

// ─────────────────────────────────────────────────────────────
// CyberPi hardware: AW9523B GPIO expander at 0x58
// P0 register: joystick + 2 buttons, all active LOW
// ─────────────────────────────────────────────────────────────
#define AW9523B_ADDR  0x58
#define AW_P0_IN_REG  0x00
#define AW_P0_CFG_REG 0x04

#define BIT_JL   (1 << 0)   // joystick left
#define BIT_JU   (1 << 1)   // joystick up
#define BIT_JR   (1 << 2)   // joystick right
#define BIT_JP   (1 << 3)   // joystick center press
#define BIT_JD   (1 << 4)   // joystick down
#define BIT_BTNB (1 << 5)   // triangle button
#define BIT_BTNA (1 << 6)   // square button (unused in new design)

#define LONG_PRESS_MS  800
#define SCROLL_DELAY   400  // ms before continuous scroll starts
#define SCROLL_FAST    100  // ms between scroll steps (fast)
#define SCROLL_SLOW    300  // ms between scroll steps (initial)

// ── Shared navigation state ──────────────────────────────────
int  gaugePage         = 0;
int  pidSelectorCursor = 0;
int  menuCursor        = 0;

// ── Deprecated — kept for compat ────────────────────────────
bool gaugeLocked       = false;

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
  Wire.write(0xFF);  // all P0 = input
  Wire.endTransmission();
}

void resetGaugePage() {
  gaugePage = 0;
  for (int i = 0; i < PID_COUNT; i++) {
    if (isPIDActive(i)) { gaugePage = i; break; }
  }
}

// ─────────────────────────────────────────────────────────────
// getIntent — reads hardware, returns one intent per call
//
// Button mapping:
//   Joystick U/D/L/R  → INTENT_UP/DOWN/LEFT/RIGHT
//   Joystick center   → INTENT_SELECT (short) / INTENT_LONG_SELECT (hold)
//   Triangle (BTNB)   → INTENT_BACK / INTENT_MENU (context set by nav.cpp)
//   Square   (BTNA)   → INTENT_MENU (opens menu, same as triangle on gauge)
// ─────────────────────────────────────────────────────────────
InputIntent getIntent() {
  static uint8_t       prevP0      = 0x7F;
  static unsigned long pressStart  = 0;
  static uint8_t       heldBit     = 0;
  static bool          longFired   = false;
  static unsigned long lastScroll  = 0;

  const uint8_t MASK = BIT_JL|BIT_JU|BIT_JR|BIT_JP|BIT_JD|BIT_BTNB|BIT_BTNA;

  uint8_t p0   = readP0() & MASK;
  uint8_t prev = prevP0   & MASK;

  uint8_t justPressed  = (uint8_t)(~p0) & prev;
  uint8_t justReleased = p0 & (uint8_t)(~prev);

  InputIntent intent = INTENT_NONE;

  // ── Press down ──────────────────────────────────────────────
  if (justPressed && heldBit == 0) {
    heldBit    = justPressed;
    pressStart = millis();
    longFired  = false;
    lastScroll = 0;
  }

  // ── Long press ──────────────────────────────────────────────
  if (heldBit && !longFired && millis() - pressStart > LONG_PRESS_MS) {
    longFired = true;
    if (heldBit & BIT_JP) {
      intent = INTENT_LONG_SELECT;
    }
  }

  // ── Continuous scroll (U/D held) ────────────────────────────
  if (!longFired && heldBit & (BIT_JU|BIT_JD) && millis() - pressStart > SCROLL_DELAY) {
    unsigned long interval = (millis() - pressStart > 800) ? SCROLL_FAST : SCROLL_SLOW;
    if (millis() - lastScroll > interval) {
      lastScroll = millis();
      if (heldBit & BIT_JU) intent = INTENT_UP;
      if (heldBit & BIT_JD) intent = INTENT_DOWN;
    }
  }

  // ── Short press (on release) ────────────────────────────────
  if (justReleased & heldBit) {
    if (!longFired) {
      uint8_t b = heldBit;
      if      (b & BIT_JU)   intent = INTENT_UP;
      else if (b & BIT_JD)   intent = INTENT_DOWN;
      else if (b & BIT_JL)   intent = INTENT_LEFT;
      else if (b & BIT_JR)   intent = INTENT_RIGHT;
      else if (b & BIT_JP)   intent = INTENT_SELECT;
      else if (b & BIT_BTNB) intent = INTENT_BACK;
      else if (b & BIT_BTNA) intent = INTENT_MENU;
    }
    heldBit = 0;
  }

  prevP0 = p0;
  return intent;
}

#endif // TARGET_CYBERPI
