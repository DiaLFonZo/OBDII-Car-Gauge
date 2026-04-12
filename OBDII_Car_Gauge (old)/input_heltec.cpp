#include "hal.h"
#ifdef TARGET_HELTEC

#include "input.h"
#include "pids.h"

// ─────────────────────────────────────────────────────────────
// Heltec WiFi LoRa 32 V3 — single user button
// GPIO0 (PRG button) — active LOW
//
// Intent mapping:
//   Short press         → INTENT_DOWN   (cycle through items)
//   Long press (>800ms) → INTENT_SELECT (confirm/action)
//   Double press        → INTENT_BACK   (go back)
//
// On gauge page:
//   Short press → next page
//   Long press  → open menu
// ─────────────────────────────────────────────────────────────

#define BTN_PIN       0     // PRG button on Heltec V3
#define LONG_PRESS_MS 800
#define DOUBLE_MS     350   // max gap between presses for double-click

int  gaugePage         = 0;
int  pidSelectorCursor = 0;
int  menuCursor        = 0;

void initInput() {
  pinMode(BTN_PIN, INPUT_PULLUP);
}

void resetGaugePage() {
  gaugePage = 0;
  for (int i = 0; i < PID_COUNT; i++) {
    if (isPIDActive(i)) { gaugePage = i; break; }
  }
}

InputIntent getIntent() {
  static bool          lastState   = HIGH;
  static unsigned long pressStart  = 0;
  static unsigned long lastRelease = 0;
  static bool          longFired   = false;
  static bool          waitDouble  = false;

  bool state = digitalRead(BTN_PIN);

  // ── Press down ──────────────────────────────────────────────
  if (state == LOW && lastState == HIGH) {
    pressStart = millis();
    longFired  = false;
  }

  // ── Long press ──────────────────────────────────────────────
  if (state == LOW && !longFired && millis() - pressStart > LONG_PRESS_MS) {
    longFired = true;
    lastState = state;
    return INTENT_SELECT;  // long = select/confirm
  }

  // ── Release ─────────────────────────────────────────────────
  if (state == HIGH && lastState == LOW) {
    lastState = state;
    if (longFired) return INTENT_NONE;

    // Check for double press
    unsigned long now = millis();
    if (waitDouble && now - lastRelease < DOUBLE_MS) {
      waitDouble = false;
      return INTENT_BACK;  // double = back
    }
    waitDouble   = true;
    lastRelease  = now;
    lastState    = state;
    return INTENT_NONE;  // wait to confirm not double
  }

  // ── Resolve single press after double-press window expires ──
  if (waitDouble && millis() - lastRelease > DOUBLE_MS) {
    waitDouble = false;
    lastState  = state;
    return INTENT_DOWN;  // single short = cycle down
  }

  lastState = state;
  return INTENT_NONE;
}

#endif // TARGET_HELTEC
