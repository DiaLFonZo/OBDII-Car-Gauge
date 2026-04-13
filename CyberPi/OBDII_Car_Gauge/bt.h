#pragma once
#include <Arduino.h>
#include <BluetoothSerial.h>
#include <Preferences.h>
#include "app_state.h"

#define MAX_SCAN_DEVICES  10   // max from a single BT scan
#define MAX_SAVED_DEVICES 10   // max devices stored in NVS

// ── Scan result (temporary, from BTSerial.discover) ──────────────
struct BTDeviceEntry {
  String name;
  String address;
};

// ── Saved device (persistent, stored in NVS) ─────────────────────
struct BTSavedDevice {
  String name;
  String mac;
};

extern BluetoothSerial BTSerial;

// ── Init ──────────────────────────────────────────────────────────
void initBT();

// ── Scan ──────────────────────────────────────────────────────────
void startScan();
void resumeScan();
bool isScanRunning();
bool isScanFinished();

void startConnectAsync(const String& mac, const String& name);
bool isConnectRunning();
bool isConnectFinished(bool &success);
int            getDeviceCount();    // scanned devices count
BTDeviceEntry* getDevice(int i);    // scanned device by index
int            getSelectedIndex();
void           setSelectedIndex(int i);

// ── Saved devices (NVS) ───────────────────────────────────────────
void loadSavedDevices();
void saveSavedDevices();
int            getSavedDeviceCount();
BTSavedDevice* getSavedDevice(int i);
int            getDefaultDeviceIndex();
void           setDefaultDevice(int i);
void           forgetDevice(int i);     // remove saved device by index
bool           hasSavedDevice();        // true if any saved device exists
String         getSavedDeviceName();    // name of default device (for display)

// ── Connect ───────────────────────────────────────────────────────
void connectToSelectedDevice(AppState &state);  // connect scan result
void connectToSavedDevice(int i, AppState &state); // connect saved device
bool tryAutoConnect(AppState &state);           // connect default on boot

// ── Connection state ──────────────────────────────────────────────
bool isBTConnected();
void disconnectBT();
void handleBT(AppState &state);

// ── Legacy aliases ────────────────────────────────────────────────
#define initBLE   initBT
#define handleBLE handleBT
#define forgetSavedDevice() forgetDevice(getDefaultDeviceIndex())
