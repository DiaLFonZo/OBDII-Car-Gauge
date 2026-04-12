#pragma once
#include <Arduino.h>
#include <NimBLEDevice.h>
#include "pids.h"
#include "ble.h"

struct OBDValues {
  float values[PID_COUNT];
  bool  hasData[PID_COUNT];
};

OBDValues& getOBDValues();
void       resetOBD();
void       handleOBD(AppState &state);
void       resetGaugePage();