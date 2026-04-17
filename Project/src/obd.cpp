#include "obd.h"
#include "ble.h"
#include "pids.h"
#include "input.h"
#include <Preferences.h>

static BLEUUID serviceUUID   ("FFF0");
static BLEUUID charWriteUUID ("FFF2");
static BLEUUID charNotifyUUID("FFF1");

static NimBLERemoteCharacteristic* pWriteChar  = nullptr;
static NimBLERemoteCharacteristic* pNotifyChar = nullptr;

// ── Combined PID list ─────────────────────────────────────────
PIDDef PIDS[MAX_PIDS];
int    PID_COUNT = 0;

void buildPIDList() {
  PID_COUNT = 0;
  for (int i = 0; i < STANDARD_PID_COUNT && PID_COUNT < MAX_PIDS; i++)
    PIDS[PID_COUNT++] = STANDARD_PIDS[i];
  for (int i = 0; i < CUSTOM_PID_COUNT && PID_COUNT < MAX_PIDS; i++)
    PIDS[PID_COUNT++] = CUSTOM_PIDS[i];
}

// ── OBD values ────────────────────────────────────────────────
static OBDValues obdValues;
OBDValues& getOBDValues() { return obdValues; }
int gaugePage = 0;
void resetGaugePage() { gaugePage = 0; }

void resetOBD() {
  pWriteChar  = nullptr;
  pNotifyChar = nullptr;
  for (int i = 0; i < MAX_PIDS; i++) {
    obdValues.values[i]  = 0.0;
    obdValues.hasData[i] = false;
  }
}

// ── Prompt-based poll state ───────────────────────────────────
static volatile bool prompt       = false;
static String        respBuf      = "";
static int           pollIndex    = 0;
static bool          pidSent      = false;
static uint8_t       skipCounters[MAX_PIDS] = {0};

// ── Notify callback ───────────────────────────────────────────
static void notifyCallback(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  for (size_t i = 0; i < len; i++) {
    char c = (char)data[i];
    if (c == '>') prompt = true;
    else          respBuf += c;
  }
}

// ── Send command ──────────────────────────────────────────────
static void sendCommand(const char* cmd) {
  if (!pWriteChar) return;
  respBuf = "";
  prompt  = false;
  String s = String(cmd) + "\r";
  pWriteChar->writeValue((uint8_t*)s.c_str(), s.length(), false);
}

static bool sendAndWait(const char* cmd, unsigned long timeoutMs = 1500) {
  sendCommand(cmd);
  unsigned long t = millis();
  while (!prompt && millis() - t < timeoutMs) delay(5);
  return prompt;
}

// ── Advance poll index ────────────────────────────────────────
static void advancePollIndex() {
  if (PID_COUNT == 0) return;
  for (int n = 0; n < PID_COUNT; n++) {
    pollIndex = (pollIndex + 1) % PID_COUNT;
    if (skipCounters[pollIndex] > 0) {
      skipCounters[pollIndex]--;
      continue;
    }
    skipCounters[pollIndex] = PIDS[pollIndex].skip;
    return;
  }
}

// ── Parse PID response ────────────────────────────────────────
static void parsePIDResponse(int pidIndex, const String& response) {
  const PIDDef& p = PIDS[pidIndex];
  String r = response;
  r.trim(); r.replace(">", ""); r.replace(" ", ""); r.toUpperCase();
  if (r.indexOf("NODATA") >= 0 || r.indexOf("ERROR") >= 0) return;

  String cmdUpper = String(p.cmd); cmdUpper.toUpperCase();
  int dataStart = -1;

  if (cmdUpper.startsWith("01")) {
    String header = "41" + cmdUpper.substring(2);
    int idx = r.indexOf(header);
    if (idx < 0) return;
    dataStart = idx + header.length();
  } else if (cmdUpper.startsWith("22")) {
    String header = "62" + cmdUpper.substring(2);
    int idx = r.indexOf(header);
    if (idx < 0) return;
    dataStart = idx + header.length();
  } else {
    int idx = r.indexOf(cmdUpper);
    if (idx < 0) return;
    dataStart = idx + cmdUpper.length();
  }

  if (dataStart < 0 || dataStart >= (int)r.length()) return;
  String data = r.substring(dataStart);
  if (data.length() < 2) return;

  byte A = strtol(data.substring(0, 2).c_str(), nullptr, 16);
  byte B = (data.length() >= 4) ? strtol(data.substring(2, 4).c_str(), nullptr, 16) : 0;
  byte C = (data.length() >= 6) ? strtol(data.substring(4, 6).c_str(), nullptr, 16) : 0;

  float val = 0.0;
  String f = String(p.formula);
  float fa = A, fb = B, fc = C;

  if      (f == "A")                val = fa;
  else if (f == "A-40")             val = fa - 40.0f;
  else if (f == "(A*256+B)/4")      val = ((fa*256)+fb)/4.0f;
  else if (f == "(A*256+B)/10-40")  val = ((fa*256)+fb)/10.0f - 40.0f;
  else if (f == "(A*256+B)/20")     val = ((fa*256)+fb)/20.0f;
  else if (f == "(A*256+B)/1000")   val = ((fa*256)+fb)/1000.0f;
  else if (f == "(A/2)-64")         val = fa/2.0f - 64.0f;
  else if (f == "(A*100)/255")      val = (fa*100.0f)/255.0f;
  else if (f == "(100/255)*C")      val = (100.0f/255.0f)*fc;
  else if (f == "B-2")              val = ((B-2)!=0) ? 1.0f : 0.0f;
  else                              val = fa;

  obdValues.values[pidIndex]  = val;
  obdValues.hasData[pidIndex] = true;
}

// ── ELM327 init ───────────────────────────────────────────────
static bool initELM(NimBLEClient* c) {
  Serial.println("[OBD] initELM: getting service FFF0...");
  NimBLERemoteService* svc = c->getService(serviceUUID);
  if (!svc) { Serial.println("[OBD] FFF0 not found, trying FFE0..."); svc = c->getService("FFE0"); }
  if (!svc) { Serial.println("[OBD] ERR: no service found"); return false; }
  Serial.println("[OBD] Service found. Getting characteristics...");

  pWriteChar  = svc->getCharacteristic(charWriteUUID);
  pNotifyChar = svc->getCharacteristic(charNotifyUUID);
  if (!pWriteChar)  pWriteChar  = svc->getCharacteristic("FFE2");
  if (!pNotifyChar) pNotifyChar = svc->getCharacteristic("FFE1");
  Serial.printf("[OBD] write=%s  notify=%s\n",
                pWriteChar  ? "OK" : "NULL",
                pNotifyChar ? "OK" : "NULL");
  if (!pWriteChar || !pNotifyChar) return false;

  if (pNotifyChar->canNotify()) pNotifyChar->subscribe(true, notifyCallback);
  Serial.println("[OBD] Subscribed. Sending ELM init sequence...");

  Serial.printf("[OBD] ATZ  → prompt=%d  resp='%s'\n", sendAndWait("ATZ",  2000), respBuf.c_str());
  Serial.printf("[OBD] ATE0 → prompt=%d\n", sendAndWait("ATE0", 500));
  Serial.printf("[OBD] ATS0 → prompt=%d\n", sendAndWait("ATS0", 300));
  Serial.printf("[OBD] ATL0 → prompt=%d\n", sendAndWait("ATL0", 300));
  Serial.printf("[OBD] ATH0 → prompt=%d\n", sendAndWait("ATH0", 300));
  Serial.printf("[OBD] ATAL → prompt=%d\n", sendAndWait("ATAL", 300));
  Serial.printf("[OBD] ATSP0→ prompt=%d\n", sendAndWait("ATSP0",500));
  Serial.println("[OBD] ELM init done");

  return true;
}

// ── handleOBD ─────────────────────────────────────────────────
void handleOBD(AppState &state) {
  if (state != STATE_INIT_ELM && state != STATE_GAUGE) return;

  NimBLEClient* c = getBLEClient();
  if (!c || !c->isConnected()) {
    resetOBD();
    state = STATE_SCANNING;
    return;
  }

  if (state == STATE_INIT_ELM) {
    buildPIDList();
    pollIndex = 0;
    pidSent   = false;
    prompt    = false;
    respBuf   = "";
    memset(skipCounters, 0, sizeof(skipCounters));
    if (!initELM(c)) {
      resetOBD();
      state = STATE_SCANNING;
      return;
    }
    prompt  = false;
    respBuf = "";
    pidSent = false;
    state   = STATE_GAUGE;
    return;
  }

  // ── Prompt-based polling ──────────────────────────────────
  if (prompt) {
    prompt  = false;
    pidSent = false;
    parsePIDResponse(pollIndex, respBuf);
    respBuf = "";

    // On the gauge page, snap back to RPM after every non-RPM poll
    // so RPM updates at ~50% of total poll bandwidth instead of 1/N
    if (gaugePage == 0 && pollIndex != 0) {
      pollIndex = 0;
    } else {
      advancePollIndex();
    }

    sendCommand(PIDS[pollIndex].cmd);
    pidSent = true;
    return;
  }

  if (!pidSent) {
    sendCommand(PIDS[pollIndex].cmd);
    pidSent = true;
  }
}