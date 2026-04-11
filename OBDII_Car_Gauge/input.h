#pragma once
#include <Arduino.h>
#include "app_state.h"

// ═══════════════════════════════════════════════════════════════
//  input.h — Hardware-agnostic input intent system
//
//  Each hardware driver translates physical inputs into intents.
//  The navigation layer (nav.cpp) only sees intents, never raw
//  button states or hardware registers.
//
//  Adding a new device = write a new input_*.cpp that produces
//  these intents. nav.cpp needs no changes.
// ═══════════════════════════════════════════════════════════════

enum InputIntent {
  INTENT_NONE,
  INTENT_UP,            // joystick up / cycle up
  INTENT_DOWN,          // joystick down / cycle down
  INTENT_LEFT,          // joystick left / swipe
  INTENT_RIGHT,         // joystick right / swipe
  INTENT_SELECT,        // center click / confirm
  INTENT_BACK,          // back / cancel
  INTENT_MENU,          // open menu (from gauge only)
  INTENT_LONG_SELECT,   // hold select / long press
};

// ── Interface every input_*.cpp must implement ──────────────────
void        initInput();    // called once in setup()
InputIntent getIntent();    // call every loop, returns latest or NONE
void        resetGaugePage();

// ── Shared navigation state ──────────────────────────────────────
extern int  gaugePage;
extern int  pidSelectorCursor;
extern int  menuCursor;
