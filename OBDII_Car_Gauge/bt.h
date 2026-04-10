#pragma once
#include <Arduino.h>
#include <BluetoothSerial.h>
#include <Preferences.h>
#include "app_state.h"   // AppState defined here now

#define MAX_DEVICES 10

struct BTDeviceEntry {
  String name;
  String address;  // MAC as string "00:1d:a5:00:12:92"
};

// Classic BT serial port
extern BluetoothSerial BTSerial;

void  initBT();
void  startScan();
void  resumeScan();
void  handleBT(AppState &state);
void  connectToSelectedDevice(AppState &state);
bool  tryAutoConnect(AppState &state);
void  forgetSavedDevice();
String getSavedDeviceName();

int          getDeviceCount();
BTDeviceEntry* getDevice(int index);
int          getSelectedIndex();
void         setSelectedIndex(int index);
bool         isBTConnected();
void         disconnectBT();

// Aliases kept during transition
#define initBLE   initBT
#define handleBLE handleBT
