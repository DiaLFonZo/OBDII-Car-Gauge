#include "obd.h"
#include "ble.h"
#include "pids.h"
#include "input.h"

PIDDef PIDS[MAX_PIDS];
int    PID_COUNT = 0;

void buildPIDList() {
  PID_COUNT = 0;
  for (int i = 0; i < STANDARD_PID_COUNT; i++)
    PIDS[PID_COUNT++] = STANDARD_PIDS[i];
  for (int i = 0; i < CUSTOM_PID_COUNT; i++)
    PIDS[PID_COUNT++] = CUSTOM_PIDS[i];
}

static BLEUUID serviceUUID   ("FFF0");
static BLEUUID charWriteUUID ("FFF2");
static BLEUUID charNotifyUUID("FFF1");

static NimBLERemoteCharacteristic* pWriteChar  = nullptr;
static NimBLERemoteCharacteristic* pNotifyChar = nullptr;

static OBDValues obdValues;
static volatile bool responseReady = false;
static String        responseBuffer = "";

int gaugePage = 0;

OBDValues& getOBDValues() { return obdValues; }

void resetGaugePage() { gaugePage = 0; }

void resetOBD() {
  pWriteChar     = nullptr;
  pNotifyChar    = nullptr;
  responseReady  = false;
  responseBuffer = "";
  for (int i = 0; i < PID_COUNT; i++) {
    obdValues.values[i]  = 0.0;
    obdValues.hasData[i] = false;
  }
}

static void notifyCallback(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  for (size_t i = 0; i < len; i++) {
    char c = (char)data[i];
    responseBuffer += c;
    if (c == '>') responseReady = true;
  }
}

static void sendCommand(const char* cmd) {
  if (!pWriteChar) return;
  responseBuffer = "";
  responseReady  = false;
  String s = String(cmd) + "\r";
  pWriteChar->writeValue((uint8_t*)s.c_str(), s.length(), false);
}

static bool sendAndWait(const char* cmd, unsigned long timeoutMs = 1000) {
  sendCommand(cmd);
  unsigned long t = millis();
  while (!responseReady && millis() - t < timeoutMs) delay(5);
  return responseReady;
}

static bool initELM(NimBLEClient* c) {
  NimBLERemoteService* svc = c->getService(serviceUUID);
  if (!svc) svc = c->getService("FFE0");
  if (!svc) return false;

  pWriteChar  = svc->getCharacteristic(charWriteUUID);
  pNotifyChar = svc->getCharacteristic(charNotifyUUID);
  if (!pWriteChar)  pWriteChar  = svc->getCharacteristic("FFE2");
  if (!pNotifyChar) pNotifyChar = svc->getCharacteristic("FFE1");
  if (!pWriteChar || !pNotifyChar) return false;

  if (pNotifyChar->canNotify()) pNotifyChar->subscribe(true, notifyCallback);

  sendCommand("ATZ");   delay(1500);
  sendCommand("ATE0");  delay(300);
  sendCommand("ATL0");  delay(300);
  sendCommand("ATS0");  delay(300);
  sendCommand("ATSP0"); delay(300);

  return true;
}

void handleOBD(AppState &state) {
  if (state != STATE_INIT_ELM && state != STATE_GAUGE) return;

  NimBLEClient* c = getBLEClient();
  if (!c || !c->isConnected()) {
    resetOBD();
    resumeScan();
    state = STATE_SCANNING;
    return;
  }

  if (state == STATE_INIT_ELM) {
      buildPIDList();
      initELM(c);
      state = STATE_GAUGE;
      return;
  }
  
}