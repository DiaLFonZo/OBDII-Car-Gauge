#include "obd.h"
#include "bt.h"
#include "pids.h"
#include "ui.h"
#include <math.h>
#include <Preferences.h>

// Forward declaration
OBDValues& getOBDValues();

// ─────────────────────────────────────────────────────────────
// Combined PID list
// ─────────────────────────────────────────────────────────────
PIDDef PIDS[MAX_PIDS];
int    PID_COUNT = 0;

void buildPIDList() {
  PID_COUNT = 0;
  // Add ALL standard PIDs — probe results shown in selector
  for (int i = 0; i < STANDARD_PID_COUNT && PID_COUNT < MAX_PIDS; i++)
    PIDS[PID_COUNT++] = STANDARD_PIDS[i];
  // Always add custom PIDs
  for (int i = 0; i < CUSTOM_PID_COUNT && PID_COUNT < MAX_PIDS; i++)
    PIDS[PID_COUNT++] = CUSTOM_PIDS[i];
  // Reset values
  OBDValues& v = getOBDValues();
  for (int i = 0; i < PID_COUNT; i++) {
    v.values[i]  = 0.0;
    v.hasData[i] = false;
  }
  Serial.print("PID list: "); Serial.println(PID_COUNT);
}

// ─────────────────────────────────────────────────────────────
// Active PID selection
// ─────────────────────────────────────────────────────────────
uint64_t activePIDs = 0ULL;

void loadActivePIDs() {
  if (PID_COUNT == 0) return;
  Preferences prefs;
  prefs.begin("pidsel", true);
  bool everSaved = prefs.getBool("_saved", false);
  activePIDs = 0;
  if (!everSaved) {
    // First run or corrupted — activate all PIDs by default
    prefs.end();
    for (int i = 0; i < PID_COUNT; i++) activePIDs |= (1ULL << i);
    saveActivePIDs();
    return;
  }
  for (int i = 0; i < PID_COUNT; i++) {
    String key = String(PIDS[i].cmd).substring(0, 6) +
                 String(PIDS[i].name).substring(0, 4);
    key.replace(" ", "_");
    bool active = prefs.getBool(key.c_str(), false);
    if (active) activePIDs |= (1ULL << i);
  }
  prefs.end();
  // Safety net — if everything somehow came back inactive, activate all
  if (activePIDs == 0) {
    for (int i = 0; i < PID_COUNT; i++) activePIDs |= (1ULL << i);
    saveActivePIDs();
  }
}

void saveActivePIDs() {
  if (PID_COUNT == 0) return;
  Preferences prefs;
  prefs.begin("pidsel", false);
  prefs.putBool("_saved", true);
  for (int i = 0; i < PID_COUNT; i++) {
    String key = String(PIDS[i].cmd).substring(0, 6) +
                 String(PIDS[i].name).substring(0, 4);
    key.replace(" ", "_");
    prefs.putBool(key.c_str(), isPIDActive(i));
  }
  prefs.end();
}

bool isPIDActive(int i) {
  if (i < 0 || i >= PID_COUNT) return false;
  return (activePIDs >> i) & 1ULL;
}

void setPIDActive(int i, bool active) {
  if (i < 0 || i >= PID_COUNT) return;
  if (active) activePIDs |=  (1ULL << i);
  else        activePIDs &= ~(1ULL << i);
}

int getActivePIDCount() {
  int count = 0;
  for (int i = 0; i < PID_COUNT; i++) if (isPIDActive(i)) count++;
  return count;
}

// ─────────────────────────────────────────────────────────────
// Car PID availability
// ─────────────────────────────────────────────────────────────
uint8_t carSupportedPIDs[16] = {0};

bool isCarPIDSupported(uint8_t pidByte) {
  if (pidByte >= 128) return false;
  return (carSupportedPIDs[pidByte / 8] >> (7 - (pidByte % 8))) & 1;
}

static void markSupported(uint8_t pidByte) {
  if (pidByte >= 128) return;
  carSupportedPIDs[pidByte / 8] |= (1 << (7 - (pidByte % 8)));
}

// ─────────────────────────────────────────────────────────────
// BT communication — replaces NimBLE characteristics
// ─────────────────────────────────────────────────────────────
static OBDValues     obdValues;
static bool          responseReady     = false;
static String        responseBuffer    = "";
bool                 anyResponseReceived = false;

// Poll state — declared here so readBTSerial can access prompt
static int     pidPollIndex  = 0;
static bool    prompt        = false;
static bool    pidSent       = false;  // true while waiting for adapter response
static uint8_t skipCounters[MAX_PIDS] = {0};

OBDValues& getOBDValues() { return obdValues; }

void resetOBD() {
  responseReady        = false;
  responseBuffer       = "";
  anyResponseReceived  = false;
  for (int i = 0; i < PID_COUNT; i++) {
    obdValues.values[i]  = 0.0;
    obdValues.hasData[i] = false;
  }
}

// Read all available bytes from BTSerial into buffer
static void readBTSerial() {
  while (bleAvailable()) {
    char c = bleRead();
    if (c == '>') {
      prompt              = true;
      responseReady       = true;
      anyResponseReceived = true;
    } else {
      responseBuffer += c;
    }
  }
}

static void sendCommand(const char* cmd) {
  responseBuffer = "";
  responseReady  = false;
  // Flush any pending bytes
  while (bleAvailable()) bleRead();
  blePrint(String(cmd) + "\r");
}

static bool sendAndWait(const char* cmd, unsigned long timeoutMs = 1500) {
  sendCommand(cmd);
  unsigned long t = millis();
  while (!responseReady && millis() - t < timeoutMs) {
    readBTSerial();
    delay(5);
  }
  return responseReady;
}

// ─────────────────────────────────────────────────────────────
// Probe scan progress callback
// ─────────────────────────────────────────────────────────────
void (*pidScanProgressCb)(int done, int total, const char* name) = nullptr;

void scanAvailablePIDs() {
  // Probe each standard PID — handled non-blocking in ino loop
  // This is kept for compatibility but main probing is in STATE_PID_SCAN
}

// ─────────────────────────────────────────────────────────────
// Parse PID response
// ─────────────────────────────────────────────────────────────
static void parsePIDResponse(int pidIndex, const String& response) {
  const PIDDef& p = PIDS[pidIndex];
  String r = response;
  r.trim(); r.replace(">", ""); r.replace(" ", ""); r.toUpperCase();

  if (r.indexOf("NODATA") >= 0 || r.indexOf("ERROR") >= 0) return;

  String cmdUpper = String(p.cmd); cmdUpper.toUpperCase();
  int dataStart = -1;
  if (cmdUpper.startsWith("01")) {
    // Mode 01 positive response: 41 + PID bytes
    String header = "41" + cmdUpper.substring(2);
    int idx = r.indexOf(header);
    if (idx < 0) return;
    dataStart = idx + header.length();
  } else if (cmdUpper.startsWith("22")) {
    // Mode 22 positive response: 62 + PID bytes (e.g. 221940 → 621940)
    String header = "62" + cmdUpper.substring(2);
    int idx = r.indexOf(header);
    if (idx < 0) return;
    dataStart = idx + header.length();
  } else {
    // Fallback: search for raw command echo
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
  float fa = (float)A, fb = (float)B, fc = (float)C;

  if      (f == "A")                 val = fa;
  else if (f == "B")                 val = fb;
  else if (f == "C")                 val = fc;
  else if (f == "A-40")              val = fa - 40.0f;
  else if (f == "A-50")              val = fa - 50.0f;
  else if (f == "A-64")              val = fa - 64.0f;
  else if (f == "A-125")             val = fa - 125.0f;
  else if (f == "A+20")              val = fa + 20.0f;
  else if (f == "A*3")               val = fa * 3.0f;
  else if (f == "A/2.55")            val = fa / 2.55f;
  else if (f == "A/2-64")            val = fa / 2.0f - 64.0f;
  else if (f == "A/1.28-100")        val = fa / 1.28f - 100.0f;
  else if (f == "(A*256+B)/4")       val = ((fa * 256.0f) + fb) / 4.0f;
  else if (f == "(A*256+B)/10-40")   val = ((fa * 256.0f) + fb) / 10.0f - 40.0f;
  else if (f == "(A*256+B)/20")      val = ((fa * 256.0f) + fb) / 20.0f;
  else if (f == "(A*256+B)/100")     val = ((fa * 256.0f) + fb) / 100.0f;
  else if (f == "(A*256+B)/128-210") val = ((fa * 256.0f) + fb) / 128.0f - 210.0f;
  else if (f == "(A*256+B)/1000")    val = ((fa * 256.0f) + fb) / 1000.0f;
  else if (f == "(A*256+B)/200")     val = ((fa * 256.0f) + fb) / 200.0f;
  else if (f == "(A*256+B)/2.55")    val = ((fa * 256.0f) + fb) / 2.55f;
  else if (f == "(A*256+B)/32768")   val = ((fa * 256.0f) + fb) / 32768.0f;
  else if (f == "(A*256+B)*10")      val = ((fa * 256.0f) + fb) * 10.0f;
  else if (f == "(A*256+B)")         val = (fa * 256.0f) + fb;
  else if (f == "(100/255)*A")       val = (100.0f / 255.0f) * fa;
  else if (f == "(100/255)*B")       val = (100.0f / 255.0f) * fb;
  else if (f == "(100/255)*C")       val = (100.0f / 255.0f) * fc;
  else if (f == "B-2")               val = ((B - 2) != 0) ? 1.0f : 0.0f;
  else { val = fa; Serial.print("Unknown formula: "); Serial.println(f); }

  obdValues.values[pidIndex]  = val;
  obdValues.hasData[pidIndex] = true;
}

// ─────────────────────────────────────────────────────────────
// Poll state — prompt-based, no timeouts
// ─────────────────────────────────────────────────────────────

void pausePolling() {
  prompt        = false;
  responseReady = false;
  responseBuffer = "";
}

// Probe a single PID — non-blocking, returns 0=waiting 1=ok 2=nodata 3=timeout
static unsigned long _probeSentAt  = 0;
static bool          _probeWaiting = false;

int probeOnePID(const char* cmd) {
  if (!bleConnected()) return 2;

  if (!_probeWaiting) {
    responseBuffer = "";
    responseReady  = false;
    while (bleAvailable()) bleRead();
    blePrint(String(cmd) + "\r");
    _probeSentAt  = millis();
    _probeWaiting = true;
    return 0;
  }

  readBTSerial();

  if (responseReady) {
    _probeWaiting = false;
    String r = responseBuffer;
    r.replace(" ", ""); r.replace(">", ""); r.toUpperCase();
    bool noData = r.indexOf("NODATA") >= 0 || r.indexOf("ERROR") >= 0 ||
                  r.indexOf("UNABLE") >= 0 || r.length() < 4;
    if (noData) return 2;
    String cu = String(cmd); cu.toUpperCase();
    if (cu.startsWith("01")) {
      return (r.indexOf("41" + cu.substring(2)) >= 0) ? 1 : 2;
    } else if (cu.startsWith("22")) {
      return (r.indexOf("62" + cu.substring(2)) >= 0) ? 1 : 2;
    }
    return 1;
  }

  if (millis() - _probeSentAt > 800) {
    _probeWaiting = false;
    return 3;
  }
  return 0;
}

void resetPollGroups() {
  pidPollIndex        = 0;
  prompt              = false;
  pidSent             = false;
  responseReady       = false;
  responseBuffer      = "";
  anyResponseReceived = false;
  memset(skipCounters, 0, sizeof(skipCounters));
  for (int i = 0; i < PID_COUNT; i++) {
    obdValues.values[i]  = 0.0;
    obdValues.hasData[i] = false;
  }
}

// Advance pidPollIndex to next active PID, wrapping around.
// If all active PIDs are in their skip countdown, fall back to
// the first active PID (lowest index) so polling never stalls.
static void advancePollIndex() {
  if (PID_COUNT == 0 || getActivePIDCount() == 0) return;

  int firstActive = -1;  // fallback

  for (int n = 0; n < PID_COUNT; n++) {
    pidPollIndex = (pidPollIndex + 1) % PID_COUNT;
    if (!isPIDActive(pidPollIndex)) continue;

    if (firstActive < 0) firstActive = pidPollIndex;  // remember first active seen

    if (skipCounters[pidPollIndex] > 0) {
      skipCounters[pidPollIndex]--;
      continue;
    }
    skipCounters[pidPollIndex] = PIDS[pidPollIndex].skip;
    return;  // found a PID due for polling
  }

  // All active PIDs were skipping — fall back to first active (e.g. RPM)
  if (firstActive >= 0) {
    pidPollIndex = firstActive;
    skipCounters[pidPollIndex] = PIDS[pidPollIndex].skip;
  }
}

static void sendCurrentPID() {
  if (!isPIDActive(pidPollIndex)) advancePollIndex();
  sendCommand(PIDS[pidPollIndex].cmd);
}

// ─────────────────────────────────────────────────────────────
// ELM327 init — Classic BT version
// ─────────────────────────────────────────────────────────────
static bool initELM() {
  if (!bleConnected()) { Serial.println("OBD: BT not connected"); return false; }

  sendAndWait("ATZ",   2000);
  sendAndWait("ATE0",  300);
  sendAndWait("ATS0",  300);
  sendAndWait("ATL0",  300);
  sendAndWait("ATH0",  300);
  sendAndWait("ATAL",  300);
  sendAndWait("ATSP0", 500);

  Serial.println("OBD: ELM327 ready");
  return true;
}

// ─────────────────────────────────────────────────────────────
// Background PID probe
// Runs silently after connect — one probe per gauge cycle
// ─────────────────────────────────────────────────────────────
static int  bgProbeIndex   = 0;    // which standard PID we're probing
static bool bgProbeDone    = false; // true once all standard PIDs probed
static bool bgProbeWaiting = false;
static unsigned long bgProbeSentAt = 0;

void resetBgProbe() {
  bgProbeIndex   = 0;
  bgProbeDone    = false;
  bgProbeWaiting = false;
  memset(carSupportedPIDs, 0, sizeof(carSupportedPIDs));
}

bool isBgProbeDone() { return bgProbeDone; }

// Called between gauge poll cycles — returns true if it used the BT line
static bool runBgProbe() {
  if (bgProbeDone) return false;
  if (bgProbeIndex >= STANDARD_PID_COUNT) {
    bgProbeDone = true;
    Serial.println("OBD: background probe complete");
    return false;
  }

  if (!bgProbeWaiting) {
    // Send probe
    responseBuffer = "";
    responseReady  = false;
    while (bleAvailable()) bleRead();
    blePrint(String(STANDARD_PIDS[bgProbeIndex].cmd) + "\r");
    bgProbeSentAt  = millis();
    bgProbeWaiting = true;
    return true;
  }

  // Waiting for response
  readBTSerial();

  if (responseReady) {
    bgProbeWaiting = false;
    String r = responseBuffer;
    r.replace(" ", ""); r.replace(">", ""); r.toUpperCase();
    bool ok = !(r.indexOf("NODATA") >= 0 || r.indexOf("ERROR") >= 0 ||
                r.indexOf("UNABLE") >= 0 || r.length() < 4);
    if (ok) {
      String cu = String(STANDARD_PIDS[bgProbeIndex].cmd); cu.toUpperCase();
      String hdr = "41" + cu.substring(2);
      if (r.indexOf(hdr) >= 0) {
        uint8_t pidByte = strtol(cu.substring(2).c_str(), nullptr, 16);
        markSupported(pidByte);
        Serial.print("BG probe OK: "); Serial.println(STANDARD_PIDS[bgProbeIndex].cmd);
      }
    }
    bgProbeIndex++;
    return false;
  }

  if (millis() - bgProbeSentAt > 800) {
    bgProbeWaiting = false;
    bgProbeIndex++;
    return false;
  }

  return true; // still waiting
}

// ─────────────────────────────────────────────────────────────
// handleOBD
// ─────────────────────────────────────────────────────────────
extern int gaugePage;

void handleOBD(AppState &state) {
  if (state != STATE_INIT_ELM && state != STATE_GAUGE &&
      state != STATE_PID_SCAN && state != STATE_MENU_PIDS) return;

  if (!bleConnected()) {
    Serial.println("OBD: BT lost");
    resetOBD();
    ui_leds_off();
    state = STATE_SCANNING;
    return;
  }

  // Always read available BT data into buffer
  readBTSerial();

  // ── Init ──────────────────────────────────────────────────
  if (state == STATE_INIT_ELM) {
    if (PID_COUNT == 0) {
      buildPIDList();
      loadActivePIDs();
    }
    pidPollIndex   = 0;
    prompt         = false;
    responseBuffer = "";
    responseReady  = false;
    memset(skipCounters, 0, sizeof(skipCounters));
    // resetBgProbe();  // bg probe disabled — using fixed PID list
    initELM();
    // Clear any leftover state from AT init before entering gauge loop
    responseBuffer = "";
    responseReady  = false;
    prompt         = false;
    pidSent        = false;
    state = STATE_GAUGE;
    return;
  }

  // ── Gauge — prompt-based poll ──────────────────────────────
  // Background probe is DISABLED — fixed PID list.
  // If nothing is active (first run / cleared prefs), activate all PIDs.
  if (getActivePIDCount() == 0) {
    for (int i = 0; i < PID_COUNT; i++) setPIDActive(i, true);
    saveActivePIDs();
  }

  // Find first active PID on first entry
  if (!isPIDActive(pidPollIndex)) advancePollIndex();

  // Got a response — parse it and send the next PID
  if (prompt) {
    prompt  = false;
    pidSent = false;  // allow sending next
    parsePIDResponse(pidPollIndex, responseBuffer);
    responseBuffer = "";
    advancePollIndex();
    sendCurrentPID();
    pidSent = true;
    return;
  }

  // Kick-off: send the first PID once — then wait for prompt
  if (!pidSent) {
    sendCurrentPID();
    pidSent = true;
  }
}
