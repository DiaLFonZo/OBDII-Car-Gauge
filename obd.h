#pragma once
#include <Arduino.h>
#include "ble.h"
#include "pids.h"

struct OBDValues {
  float values[PID_COUNT];
  bool  hasData[PID_COUNT];
};

OBDValues& getOBDValues();
void       handleOBD(AppState &state);
void       resetOBD();
