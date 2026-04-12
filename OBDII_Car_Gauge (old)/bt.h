#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include "app_state.h"

// ═══════════════════════════════════════════════════════════════
//  bt.h — BLE client for ELM327 OBD adapters (Veepeak, etc.)
//
//  Uses NimBLE-Arduino library — works on ESP32 and ESP32-S3.
//  ELM327 BLE adapters use a UART-over-BLE service:
//    Service:  FFF0
//    Notify:   FFF1  (adapter → ESP32, receive responses)
//    Write:    FFF2  (ESP32 → adapter, send commands)
// ═══════════════════════════════════════════════════════════════

#define MAX_SCAN_DEVICES  10
#define MAX_SAVED_DEVICES 10

// ── Scan result (temporary) ──────────────────────────────────────
struct BTDeviceEntry {
  String  name;
  String  address;
  uint8_t addrType = 0;
};

// ── Saved device (persistent, NVS) ──────────────────────────────
struct BTSavedDevice {
  String name;
  String mac;
  uint8_t addrType = 0;  // 0=public, 1=random
};

// ── Stream interface (used by obd.cpp) ──────────────────────────
// obd.cpp calls these instead of BTSerial.xxx
bool   bleAvailable();
char   bleRead();
void   blePrint(const String& s);
bool   bleConnected();

// ── Init ─────────────────────────────────────────────────────────
void initBT();

// ── Scan ─────────────────────────────────────────────────────────
void           startScan();
void           resumeScan();
bool           isScanRunning();
bool           isScanFinished();
int            getDeviceCount();
BTDeviceEntry* getDevice(int i);
int            getSelectedIndex();
void           setSelectedIndex(int i);

// ── Saved devices ────────────────────────────────────────────────
void           loadSavedDevices();
void           saveSavedDevices();
int            getSavedDeviceCount();
BTSavedDevice* getSavedDevice(int i);
int            getDefaultDeviceIndex();
void           setDefaultDevice(int i);
void           forgetDevice(int i);
bool           hasSavedDevice();
String         getSavedDeviceName();

// ── Connect ──────────────────────────────────────────────────────
void startConnectAsync(const String& mac, const String& name);
bool isConnectRunning();
bool isConnectFinished(bool& success);
bool tryAutoConnect(AppState& state);
void connectToSavedDevice(int i, AppState& state);
void connectToSelectedDevice(AppState& state);

// ── Connection health ────────────────────────────────────────────
bool   isBTConnected();
void   disconnectBT();
void   handleBT(AppState& state);
void   addSavedDevice(const String& name, const String& mac);

// ── Legacy aliases ────────────────────────────────────────────────
#define initBLE   initBT
#define handleBLE handleBT
