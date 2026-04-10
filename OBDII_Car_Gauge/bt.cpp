#include "bt.h"
#include "obd.h"
#include "input.h"

BluetoothSerial BTSerial;

// ── Scan results (temporary) ──────────────────────────────────────
static BTDeviceEntry scanList[MAX_SCAN_DEVICES];
static int           scanCount     = 0;
static int           selectedIndex = 0;
static bool          btConnected   = false;

// ── Saved devices (persistent) ───────────────────────────────────
static BTSavedDevice savedDevices[MAX_SAVED_DEVICES];
static int           savedCount        = 0;
static int           defaultDeviceIdx  = 0;

// ── Saved device accessors ────────────────────────────────────────
int            getSavedDeviceCount()   { return savedCount; }
BTSavedDevice* getSavedDevice(int i)   { return (i>=0 && i<savedCount) ? &savedDevices[i] : nullptr; }
int            getDefaultDeviceIndex() { return defaultDeviceIdx; }
bool           hasSavedDevice()        { return savedCount > 0; }

String getSavedDeviceName() {
  if (savedCount == 0) return "";
  BTSavedDevice* d = getSavedDevice(defaultDeviceIdx);
  if (!d) return "";
  return d->name != "" ? d->name : d->mac;
}

void setDefaultDevice(int i) {
  if (i >= 0 && i < savedCount) {
    defaultDeviceIdx = i;
    saveSavedDevices();
  }
}

void loadSavedDevices() {
  Preferences prefs;
  prefs.begin("devices", true);
  savedCount       = prefs.getInt("count",   0);
  defaultDeviceIdx = prefs.getInt("default", 0);
  if (savedCount > MAX_SAVED_DEVICES) savedCount = 0;
  for (int i = 0; i < savedCount; i++) {
    savedDevices[i].name = prefs.getString(("name" + String(i)).c_str(), "");
    savedDevices[i].mac  = prefs.getString(("mac"  + String(i)).c_str(), "");
  }
  prefs.end();
  Serial.printf("Loaded %d saved devices, default=%d\n", savedCount, defaultDeviceIdx);
}

void saveSavedDevices() {
  Preferences prefs;
  prefs.begin("devices", false);
  prefs.putInt("count",   savedCount);
  prefs.putInt("default", defaultDeviceIdx);
  for (int i = 0; i < savedCount; i++) {
    prefs.putString(("name" + String(i)).c_str(), savedDevices[i].name);
    prefs.putString(("mac"  + String(i)).c_str(), savedDevices[i].mac);
  }
  prefs.end();
}

// Add or update a device in saved list — called on successful connect
static void addSavedDevice(const String& name, const String& mac) {
  // Check if already saved
  for (int i = 0; i < savedCount; i++) {
    if (savedDevices[i].mac == mac) {
      // Update name if it changed, set as default
      savedDevices[i].name = name;
      defaultDeviceIdx = i;
      saveSavedDevices();
      return;
    }
  }
  // New device — add if space
  if (savedCount < MAX_SAVED_DEVICES) {
    savedDevices[savedCount].name = name;
    savedDevices[savedCount].mac  = mac;
    defaultDeviceIdx = savedCount;
    savedCount++;
    saveSavedDevices();
  }
  // If full, replace oldest non-default (index 0 if not default)
  // Simple strategy: replace slot 0 and shift
  // For now just overwrite slot 0 if full
}

void forgetDevice(int i) {
  if (i < 0 || i >= savedCount) return;
  // Shift remaining devices down
  for (int j = i; j < savedCount - 1; j++) {
    savedDevices[j] = savedDevices[j+1];
  }
  savedCount--;
  if (defaultDeviceIdx >= savedCount) defaultDeviceIdx = max(0, savedCount-1);
  saveSavedDevices();
  Serial.printf("Forgot device %d, %d remaining\n", i, savedCount);
}

// ── Scan ──────────────────────────────────────────────────────────
int            getDeviceCount()   { return scanCount; }
BTDeviceEntry* getDevice(int i)   { return (i>=0 && i<scanCount) ? &scanList[i] : nullptr; }
int            getSelectedIndex() { return selectedIndex; }
void           setSelectedIndex(int i) { selectedIndex = i; }

// ── Async scan support ────────────────────────────────────────────
static volatile bool scanRunning = false;
static volatile bool scanFinished = false;

static void scanTask(void* param) {
  BTScanResults* results = BTSerial.discover(5000);
  if (results) {
    for (int i = 0; i < results->getCount() && scanCount < MAX_SCAN_DEVICES; i++) {
      BTAdvertisedDevice* dev = results->getDevice(i);
      scanList[scanCount].name    = dev->getName().c_str();
      scanList[scanCount].address = dev->getAddress().toString().c_str();
      if (scanList[scanCount].name == "")
        scanList[scanCount].name = scanList[scanCount].address;
      scanCount++;
    }
  }
  Serial.printf("Scan done: %d devices found\n", scanCount);
  scanRunning  = false;
  scanFinished = true;
  vTaskDelete(nullptr);
}

void startScan() {
  if (scanRunning) return;
  scanCount     = 0;
  selectedIndex = 0;
  scanRunning   = true;
  scanFinished  = false;
  xTaskCreatePinnedToCore(scanTask, "BTScan", 4096, nullptr, 1, nullptr, 0);
}

bool isScanRunning()  { return scanRunning; }
bool isScanFinished() {
  if (scanFinished) { scanFinished = false; return true; }
  return false;
}

void resumeScan() { startScan(); }

// ── Async connect support ────────────────────────────────────────
static volatile bool connectRunning  = false;
static volatile bool connectDone     = false;
static volatile bool connectSuccess  = false;
static String        connectMac_g    = "";
static String        connectName_g   = "";
static AppState*     connectStatePtr = nullptr;

static void connectTask(void* param) {
  uint8_t macBytes[6] = {0};
  String s = connectMac_g;
  for (int i = 0; i < 6; i++) {
    int idx = s.indexOf(':');
    String b = (idx == -1) ? s : s.substring(0, idx);
    macBytes[i] = strtol(b.c_str(), nullptr, 16);
    if (idx != -1) s = s.substring(idx + 1);
  }
  connectSuccess = BTSerial.connect(macBytes);
  if (connectSuccess) {
    btConnected = true;
    Serial.println("BT connected (async)");
    addSavedDevice(connectName_g, connectMac_g);
  } else {
    Serial.println("BT connect failed (async)");
  }
  connectRunning = false;
  connectDone    = true;
  vTaskDelete(nullptr);
}

void startConnectAsync(const String& mac, const String& name) {
  if (connectRunning) return;
  connectMac_g    = mac;
  connectName_g   = name;
  connectRunning  = true;
  connectDone     = false;
  connectSuccess  = false;
  BTSerial.disconnect();
  btConnected = false;
  xTaskCreatePinnedToCore(connectTask, "BTConn", 4096, nullptr, 1, nullptr, 0);
}

bool isConnectRunning()  { return connectRunning; }
bool isConnectFinished(bool &success) {
  if (connectDone) {
    connectDone = false;
    success = connectSuccess;
    return true;
  }
  return false;
}

// ── Init ──────────────────────────────────────────────────────────
void initBT() {
  BTSerial.begin("OBD2Gauge", true);
  BTSerial.setPin("1234", 4);
  loadSavedDevices();
}

// ── Internal connect by MAC ───────────────────────────────────────
static bool connectByAddress(const String& mac, const String& name, AppState &state) {
  BTSerial.disconnect();
  btConnected = false;

  uint8_t macBytes[6] = {0};
  String s = mac;
  for (int i = 0; i < 6; i++) {
    int idx = s.indexOf(':');
    String b = (idx == -1) ? s : s.substring(0, idx);
    macBytes[i] = strtol(b.c_str(), nullptr, 16);
    if (idx != -1) s = s.substring(idx+1);
  }

  Serial.print("BT connecting to "); Serial.println(mac);
  if (BTSerial.connect(macBytes)) {
    btConnected = true;
    resetOBD();
    resetGaugePage();
    state = STATE_INIT_ELM;
    addSavedDevice(name, mac);
    Serial.println("BT connected");
    return true;
  }
  Serial.println("BT connect failed");
  // Don't change state here — caller decides what to do on failure
  return false;
}

// ── Public connect functions ──────────────────────────────────────
void connectToSelectedDevice(AppState &state) {
  BTDeviceEntry* dev = getDevice(selectedIndex);
  if (!dev) { state = STATE_MENU_CONNECT; return; }
  if (!connectByAddress(dev->address, dev->name, state))
    state = STATE_MENU_CONNECT;
}

void connectToSavedDevice(int i, AppState &state) {
  BTSavedDevice* dev = getSavedDevice(i);
  if (!dev) { state = STATE_MENU_CONNECT; return; }
  if (!connectByAddress(dev->mac, dev->name, state))
    state = STATE_MENU_CONNECT;
}

bool tryAutoConnect(AppState &state) {
  if (savedCount == 0) return false;
  BTSavedDevice* dev = getSavedDevice(defaultDeviceIdx);
  if (!dev || dev->mac == "") return false;
  Serial.print("Auto-connect to "); Serial.println(dev->mac);
  // On failure, state unchanged — stays in STATE_GAUGE (offline)
  return connectByAddress(dev->mac, dev->name, state);
}

// ── Connection health ─────────────────────────────────────────────
bool isBTConnected() { return btConnected && BTSerial.connected(); }

void disconnectBT() {
  BTSerial.disconnect();
  btConnected = false;
}

void handleBT(AppState &state) {
  if (state == STATE_GAUGE || state == STATE_INIT_ELM) {
    if (btConnected && !BTSerial.connected()) {
      Serial.println("BT disconnected");
      btConnected = false;
      resetOBD();
      state = STATE_GAUGE;
    }
  }
}
