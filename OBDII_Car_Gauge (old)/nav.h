#pragma once
#include "input.h"
#include "app_state.h"

// ═══════════════════════════════════════════════════════════════
//  nav.h — Navigation logic
//
//  Translates InputIntents into AppState transitions.
//  Hardware-agnostic — works identically on CyberPi, Heltec,
//  Waveshare, or any future target.
// ═══════════════════════════════════════════════════════════════

void handleNav(InputIntent intent, AppState &state);
