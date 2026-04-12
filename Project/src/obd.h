#pragma once
#include <Arduino.h>
#include <NimBLEDevice.h>
#include "pids.h"
#include "ble.h"

struct OBDValues {
  float values[MAX_PIDS];
  bool  hasData[MAX_PIDS];
};

OBDValues& getOBDValues();
void       resetOBD();
void       handleOBD(AppState &state);
void       resetGaugePage();