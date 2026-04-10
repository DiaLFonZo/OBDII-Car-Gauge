#pragma once

// ═══════════════════════════════════════════════════════════════
//  app_state.h — Application state machine definition
//
//  Single source of truth for AppState.
//  Included by all layers that need to know the current state.
//  Does NOT include any hardware or display headers.
// ═══════════════════════════════════════════════════════════════

enum AppState {
  // ── Connection ──────────────────────────────────────────────
  STATE_AUTO_CONNECT,   // background connect attempt on startup
  STATE_INIT_ELM,       // ELM327 AT init sequence

  // ── Normal operation ────────────────────────────────────────
  STATE_GAUGE,          // live gauge display — default state

  // ── Menu system (Square button) ─────────────────────────────
  STATE_MENU,           // top-level menu
  STATE_MENU_PIDS,      // PID selector (toggle active PIDs)
  STATE_MENU_CONNECT,   // BT scan / connect / forget
  STATE_MENU_SETTINGS,  // warn thresholds, brightness, etc.
  STATE_MENU_DEFAULTS,  // placeholder: default reading config

  // ── Legacy — no longer used, kept to avoid breaking old references ─
  STATE_SCANNING,       // retired — use STATE_MENU_CONNECT
  STATE_CONNECTING,     // retired — connecting happens inside STATE_MENU_CONNECT
  STATE_PID_SCAN        // retired — use STATE_MENU_PIDS
};
