#include "obd.h"
#include "ble.h"
#include "pids.h"
#include "input.h"

static BLEUUID serviceUUID   ("FFF0");
static BLEUUID charWriteUUID ("FFF2");
static BLEUUID charNotifyUUID("FFF1");

static NimBLERemoteCharacteristic* pWriteChar  = nullptr;
static NimBLERemoteCharacteristic* pNotifyChar = nullptr;

static OBDValues obdValues;
static volatile bool responseReady = false;
static String        responseBuffer = "";

OBDValues& getOBDValues() { return obdValues; }

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

// ─────────────────────────────
// Notify callback
// ─────────────────────────────

static void notifyCallback(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  for (size_t i = 0; i < len; i++) {
    char c = (char)data[i];
    responseBuffer += c;
    if (c == '>') responseReady = true;
  }
}

// ─────────────────────────────
// sendCommand — write THEN clear buffer, exactly like original
// ─────────────────────────────

static void sendCommand(const char* cmd) {
  if (!pWriteChar) return;
  responseBuffer = "";
  responseReady  = false;
  String s = String(cmd) + "\r";
  pWriteChar->writeValue((uint8_t*)s.c_str(), s.length(), false);
}

// ─────────────────────────────
// sendAndWait — send then wait for '>' prompt
// Used for polling (same pattern as original loop)
// ─────────────────────────────

static bool sendAndWait(const char* cmd, unsigned long timeoutMs = 1000) {
  sendCommand(cmd);
  unsigned long t = millis();
  while (!responseReady && millis() - t < timeoutMs) {
    handleButton();   // keep button responsive during blocking wait
    delay(5);
  }
  return responseReady;
}

// ─────────────────────────────
// Parse — identical logic to original
// ─────────────────────────────

static void parsePIDResponse(int pidIndex, const String& response) {
  const PIDDef& p = PIDS[pidIndex];

  String r = response;
  r.trim();
  r.replace(">", "");
  r.replace(" ", "");
  r.toUpperCase();

  if (r.indexOf("NODATA") >= 0 || r.indexOf("ERROR") >= 0) return;

  // "010C" -> pid="0C", header="410C"  (same as original substring(2))
  String cmdUpper = String(p.cmd);
  cmdUpper.toUpperCase();

  int dataStart = -1;

  if (cmdUpper.startsWith("01")) {
    String header = "41" + cmdUpper.substring(2);
    int idx = r.indexOf(header);
    if (idx < 0) return;
    dataStart = idx + 4;   // skip "41XX" = 4 chars, same as original idx+4
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
  switch (p.equation) {
    case EQ_RPM:        val = ((A * 256.0) + B) / 4.0;     break;
    case EQ_A:          val = (float)A;                     break;
    case EQ_A_MINUS_40: val = (float)A - 40.0;             break;
    case EQ_A_OFFSET:   val = (float)A + p.offset;         break;
    case EQ_B_BOOL:     val = ((B - 2) != 0) ? 1.0 : 0.0; break;
    case EQ_C_PERCENT:  val = (100.0 / 255.0) * C;         break;
    case EQ_CUSTOM_FF:  val = (float)A + p.offset;         break;
  }

  obdValues.values[pidIndex]  = val;
  obdValues.hasData[pidIndex] = true;

  Serial.print("OBD "); Serial.print(p.name);
  Serial.print(" = "); Serial.println(val);
}

// ─────────────────────────────
// Poll groups — one command can serve multiple PIDs (018B)
// ─────────────────────────────

struct PollGroup {
  const char* cmd;
  int indices[PID_COUNT];
  int count;
};
static PollGroup pollGroups[PID_COUNT];
static int       pollGroupCount = 0;
static unsigned long lastPoll   = 0;
static int           groupIndex = 0;

static void buildPollGroups() {
  pollGroupCount = 0;
  for (int i = 0; i < PID_COUNT; i++) {
    bool found = false;
    for (int g = 0; g < pollGroupCount; g++) {
      if (strcmp(pollGroups[g].cmd, PIDS[i].cmd) == 0) {
        pollGroups[g].indices[pollGroups[g].count++] = i;
        found = true; break;
      }
    }
    if (!found) {
      pollGroups[pollGroupCount].cmd        = PIDS[i].cmd;
      pollGroups[pollGroupCount].indices[0] = i;
      pollGroups[pollGroupCount].count      = 1;
      pollGroupCount++;
    }
  }
  Serial.print("OBD poll groups: "); Serial.println(pollGroupCount);
}

// ─────────────────────────────
// Init ELM327 — mirrors original exactly:
// sendCommand + delay (not sendAndWait) so ATZ banner doesn't block us
// ─────────────────────────────

static bool initELM(NimBLEClient* c) {
  NimBLERemoteService* svc = c->getService(serviceUUID);
  if (!svc) svc = c->getService("FFE0");
  if (!svc) { Serial.println("OBD: service not found"); return false; }

  pWriteChar  = svc->getCharacteristic(charWriteUUID);
  pNotifyChar = svc->getCharacteristic(charNotifyUUID);
  if (!pWriteChar)  pWriteChar  = svc->getCharacteristic("FFE2");
  if (!pNotifyChar) pNotifyChar = svc->getCharacteristic("FFE1");
  if (!pWriteChar || !pNotifyChar) { Serial.println("OBD: chars not found"); return false; }

  if (pNotifyChar->canNotify()) pNotifyChar->subscribe(true, notifyCallback);

  // Mirror original exactly — sendCommand + fixed delay, no waiting for '>'
  sendCommand("ATZ");   delay(1500);
  sendCommand("ATE0");  delay(300);
  sendCommand("ATL0");  delay(300);
  sendCommand("ATS0");  delay(300);
  sendCommand("ATSP0"); delay(300);

  Serial.println("OBD: ELM327 ready");
  return true;
}

// ─────────────────────────────
// handleOBD
// ─────────────────────────────

extern int gaugePage;

void handleOBD(AppState &state) {
  if (state != STATE_INIT_ELM && state != STATE_GAUGE) return;

  NimBLEClient* c = getBLEClient();
  if (!c || !c->isConnected()) {
    Serial.println("OBD: lost connection");
    resetOBD(); resumeScan(); state = STATE_SCANNING; return;
  }

  if (state == STATE_INIT_ELM) {
    buildPollGroups();
    lastPoll   = 0;
    groupIndex = 0;
    initELM(c);
    state = STATE_GAUGE;
    return;
  }

  // Poll one group per call — 400ms interval, same as original
  if (pollGroupCount == 0) return;   // guard: buildPollGroups not yet called
  if (millis() - lastPoll < 400) return;
  lastPoll = millis();

  if (groupIndex >= pollGroupCount) groupIndex = 0;  // guard stale index

  if (sendAndWait(pollGroups[groupIndex].cmd, 1000)) {
    for (int i = 0; i < pollGroups[groupIndex].count; i++) {
      parsePIDResponse(pollGroups[groupIndex].indices[i], responseBuffer);
    }
  }

  groupIndex = (groupIndex + 1) % pollGroupCount;
}
