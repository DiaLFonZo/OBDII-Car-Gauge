#pragma once
#include <Arduino.h>
#include "bt.h"
#include "pids.h"

struct OBDValues {
  float values[64];
  bool  hasData[64];
};

extern bool     anyResponseReceived;
extern uint64_t activePIDs;
extern uint8_t  carSupportedPIDs[16];

OBDValues& getOBDValues();
void       resetOBD();
void       resetPollGroups();
void       pausePolling();
void       resetBgProbe();
bool       isBgProbeDone();
void       handleOBD(AppState &state);
int        probeOnePID(const char* cmd);

void loadActivePIDs();
void saveActivePIDs();
bool isPIDActive(int i);
void setPIDActive(int i, bool active);
int  getActivePIDCount();
bool isCarPIDSupported(uint8_t pidByte);
void scanAvailablePIDs();
void buildPIDList();

extern void (*pidScanProgressCb)(int done, int total, const char* name);
