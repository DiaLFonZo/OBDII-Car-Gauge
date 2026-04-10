#pragma once
#include <Arduino.h>
#include "app_state.h"

// ═══════════════════════════════════════════════════════════════
//  ui.h — Abstract UI interface
//
//  The application layer calls only these functions.
//  Each display target implements this interface in its own file:
//    ui_cyberpi.cpp   — 128x128 ST7735  (current)
//    ui_waveshare.cpp — 480x480 round   (future)
//
//  Rules for implementations:
//  - Never include bt.h, obd.h, or input.h directly
//  - Read data through the accessor functions declared below
//  - All pixel coordinates are internal to the implementation
// ═══════════════════════════════════════════════════════════════

// ── Theme ─────────────────────────────────────────────────────
void ui_setTheme(bool dark);   // true=dark, false=light — saves to NVS
bool ui_isDarkTheme();

// ── Lifecycle ────────────────────────────────────────────────
void ui_init();          // called once in setup()

// ── Gauge ────────────────────────────────────────────────────
void ui_gauge(int pidIndex);   // draw live gauge for one PID

// ── Menu system ──────────────────────────────────────────────
void ui_menu(int selection, bool connected);        // top-level menu
void ui_menuPIDs(int cursor);                       // PID toggle list
void ui_menuConnect(int deviceCount, int selected); // BT scan list
void ui_menuSettings(int cursor);                   // settings list

// ── Status / transitional screens ────────────────────────────
void ui_autoConnect();         // reconnecting animation
void ui_resetAutoConnect();    // reset animation timer
void ui_connecting();          // connecting to selected device

// ── LED bar (optional — no-op on displays without LEDs) ──────
void ui_leds(float pct, float warnPct);  // 0.0-1.0 value bar
void ui_leds_off();

// ── Utility ───────────────────────────────────────────────────
void ui_flush();            // force display update
void ui_scanOverlay();      // diagonal bar animation while scanning
void ui_connectOverlay();   // diagonal bar animation while connecting
void ui_connectOverlay();   // diagonal bar animation while connecting
void VextON();   // no-op on CyberPi

// ── Legacy update dispatcher (will be removed in step 2) ─────
void ui_updateLegacy(int state);

// ── Data accessors used by UI implementations ────────────────
// These are defined in their respective modules and used by
// ui_*.cpp to read data without coupling to display logic.
// Declared here so ui_*.cpp only needs to include ui.h.

// From obd.cpp
struct OBDValues;
OBDValues& getOBDValues();
bool isPIDActive(int i);
void setPIDActive(int i, bool active);
int  getActivePIDCount();
void loadActivePIDs();
void saveActivePIDs();
void resetOBD();
void resetPollGroups();
void handleOBD(AppState &state);
extern bool anyResponseReceived;

// From bt.cpp — scan results
struct BTDeviceEntry;
BTDeviceEntry* getDevice(int index);
int    getDeviceCount();
int    getSelectedIndex();
void   setSelectedIndex(int i);

// From bt.cpp — saved devices
struct BTSavedDevice;
BTSavedDevice* getSavedDevice(int i);
int    getSavedDeviceCount();
int    getDefaultDeviceIndex();
void   setDefaultDevice(int i);
void   forgetDevice(int i);
bool   hasSavedDevice();
String getSavedDeviceName();

// From bt.cpp — connect
bool   isBTConnected();
void   disconnectBT();
void   resumeScan();
void   startScan();
void   connectToSelectedDevice(AppState &state);
void   connectToSavedDevice(int i, AppState &state);
bool   tryAutoConnect(AppState &state);
void   handleBT(AppState &state);

// From pids.h
struct PIDDef;
extern PIDDef PIDS[];
extern int    PID_COUNT;
void buildPIDList();
