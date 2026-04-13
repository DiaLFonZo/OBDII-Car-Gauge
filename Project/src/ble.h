#pragma once
#include <Arduino.h>
#include <NimBLEDevice.h>

#define MAX_DEVICES 15

enum AppState {
  STATE_BOOT,
  STATE_AUTO_CONNECT,
  STATE_SCANNING,
  STATE_CONNECTING,
  STATE_INIT_ELM,
  STATE_GAUGE
};

struct BLEDeviceEntry {
  String name;
  String address;
};

void            initBLE();
void            startScan();
void            resumeScan();
void            stopScan();
void            handleBLE(AppState &state);
void            connectToSelectedDevice(AppState &state);
bool            tryAutoConnect(AppState &state);
void            forgetSavedDevice();
String          getSavedDeviceName();

int             getDeviceCount();
BLEDeviceEntry* getDevice(int index);
int             getSelectedIndex();
void            nextDevice();
void            setSelectedIndex(int index);
NimBLEClient*   getBLEClient();