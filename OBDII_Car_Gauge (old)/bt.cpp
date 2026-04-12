// ═══════════════════════════════════════════════════════════════
//  bt.cpp — NimBLE-Arduino 2.x client for ELM327 OBD BLE adapters
//
//  Veepeak OBDCheck BLE service layout:
//    Service UUID:  FFF0
//    RX char UUID:  FFF1  (notify — adapter sends responses here)
//    TX char UUID:  FFF2  (write — we send AT commands here)
//
//  Requires: NimBLE-Arduino 2.x (Library Manager)
// ═══════════════════════════════════════════════════════════════

#include "bt.h"
#include "obd.h"
#include "input.h"   // for resetGaugePage
#include <NimBLEDevice.h>
#include <Preferences.h>

#define OBD_SERVICE_UUID  "FFF0"
#define OBD_NOTIFY_UUID   "FFF1"
#define OBD_WRITE_UUID    "FFF2"

// ── Receive ring buffer ─────────────────────────────────────────
#define RX_BUF_SIZE 512
static char     rxBuf[RX_BUF_SIZE];
static volatile int rxHead = 0;
static volatile int rxTail = 0;

static void rxPush(const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    int next = (rxHead + 1) % RX_BUF_SIZE;
    if (next != rxTail) { rxBuf[rxHead] = (char)data[i]; rxHead = next; }
  }
}

bool bleAvailable() { return rxHead != rxTail; }
char bleRead() {
  if (rxHead == rxTail) return 0;
  char c = rxBuf[rxTail];
  rxTail = (rxTail + 1) % RX_BUF_SIZE;
  return c;
}

// ── BLE globals ─────────────────────────────────────────────────
static NimBLEClient*               bleClient = nullptr;
static NimBLERemoteCharacteristic* txChar    = nullptr;
static NimBLERemoteCharacteristic* rxChar    = nullptr;
static bool                        connected = false;

bool bleConnected() { return connected && bleClient && bleClient->isConnected(); }

void blePrint(const String& s) {
  if (!bleConnected() || !txChar) return;
  txChar->writeValue((const uint8_t*)s.c_str(), s.length(), false);
}

// ── Notify callback ─────────────────────────────────────────────
static void notifyCallback(NimBLERemoteCharacteristic* /*c*/,
                            const uint8_t* data, size_t length, bool /*isNotify*/) {
  rxPush(data, length);
}

// ── Client callbacks (NimBLE 2.x signature) ────────────────────
class ClientCB : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient* /*client*/, int /*reason*/) override {
    connected = false;
    txChar = nullptr;
    rxChar = nullptr;
    Serial.println("BLE disconnected");
  }
};
static ClientCB clientCB;

// ── Scan results ─────────────────────────────────────────────────
static BTDeviceEntry scanList[MAX_SCAN_DEVICES];
static int           scanCount     = 0;
static int           selectedIndex = 0;
static volatile bool scanRunning   = false;
static volatile bool scanDone      = false;

int            getDeviceCount()        { return scanCount; }
BTDeviceEntry* getDevice(int i)        { return (i>=0 && i<scanCount) ? &scanList[i] : nullptr; }
int            getSelectedIndex()      { return selectedIndex; }
void           setSelectedIndex(int i) { selectedIndex = i; }
bool           isScanRunning()         { return scanRunning; }
bool           isScanFinished()        { if (scanDone) { scanDone=false; return true; } return false; }

// ── Scan callbacks (NimBLE 2.x — inherits NimBLEScanCallbacks) ──
class ScanCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    if (scanCount >= MAX_SCAN_DEVICES) return;
    std::string n = dev->getName();
    scanList[scanCount].name    = n.length() > 0 ? n.c_str()
                                 : dev->getAddress().toString().c_str();
    scanList[scanCount].address = dev->getAddress().toString().c_str();
    scanList[scanCount].addrType = dev->getAddress().getType();
    scanCount++;
  }
};
static ScanCB scanCB;

static void scanTask(void*) {
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCB, false);
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  scan->start(5000, false);  // 5000ms, blocking
  Serial.printf("BLE scan done: %d devices\n", scanCount);
  scanRunning = false;
  scanDone    = true;
  vTaskDelete(nullptr);
}

void startScan() {
  if (scanRunning) return;
  scanCount     = 0;
  selectedIndex = 0;
  scanRunning   = true;
  scanDone      = false;
  xTaskCreatePinnedToCore(scanTask, "BLEScan", 4096, nullptr, 1, nullptr, 0);
}
void resumeScan() { startScan(); }

// ── Saved devices ────────────────────────────────────────────────
static BTSavedDevice savedDevices[MAX_SAVED_DEVICES];
static int           savedCount       = 0;
static int           defaultDeviceIdx = 0;

int            getSavedDeviceCount()   { return savedCount; }
BTSavedDevice* getSavedDevice(int i)   { return (i>=0&&i<savedCount) ? &savedDevices[i] : nullptr; }
int            getDefaultDeviceIndex() { return defaultDeviceIdx; }
bool           hasSavedDevice()        { return savedCount > 0; }

String getSavedDeviceName() {
  BTSavedDevice* d = getSavedDevice(defaultDeviceIdx);
  return d ? (d->name != "" ? d->name : d->mac) : "";
}

void setDefaultDevice(int i) {
  if (i >= 0 && i < savedCount) { defaultDeviceIdx = i; saveSavedDevices(); }
}

void loadSavedDevices() {
  Preferences p; p.begin("devices", true);
  savedCount       = p.getInt("count",   0);
  defaultDeviceIdx = p.getInt("default", 0);
  if (savedCount > MAX_SAVED_DEVICES) savedCount = 0;
  for (int i = 0; i < savedCount; i++) {
    savedDevices[i].name = p.getString(("n"+String(i)).c_str(), "");
    savedDevices[i].mac  = p.getString(("m"+String(i)).c_str(), "");
  }
  p.end();
  Serial.printf("Loaded %d saved devices\n", savedCount);
}

void saveSavedDevices() {
  Preferences p; p.begin("devices", false);
  p.putInt("count",   savedCount);
  p.putInt("default", defaultDeviceIdx);
  for (int i = 0; i < savedCount; i++) {
    p.putString(("n"+String(i)).c_str(), savedDevices[i].name);
    p.putString(("m"+String(i)).c_str(), savedDevices[i].mac);
  }
  p.end();
}

void addSavedDevice(const String& name, const String& mac, uint8_t addrType) {
  for (int i = 0; i < savedCount; i++) {
    if (savedDevices[i].mac == mac) {
      savedDevices[i].name = name;
      savedDevices[i].addrType = addrType;
      defaultDeviceIdx = i;
      saveSavedDevices();
      return;
    }
  }
  if (savedCount < MAX_SAVED_DEVICES) {
    savedDevices[savedCount] = { name, mac, addrType };
    defaultDeviceIdx = savedCount++;
    saveSavedDevices();
  }
}

void forgetDevice(int i) {
  if (i < 0 || i >= savedCount) return;
  for (int j = i; j < savedCount-1; j++) savedDevices[j] = savedDevices[j+1];
  savedCount--;
  if (defaultDeviceIdx >= savedCount) defaultDeviceIdx = max(0, savedCount-1);
  saveSavedDevices();
}

// ── Core connect ─────────────────────────────────────────────────
static bool connectToAddress(const String& mac, const String& name) {
  rxHead = rxTail = 0;

  if (!bleClient) {
    bleClient = NimBLEDevice::createClient();
    bleClient->setClientCallbacks(&clientCB, false);
  } else if (bleClient->isConnected()) {
    bleClient->disconnect();
    delay(300);
  }

  // NimBLE 2.x: NimBLEAddress needs std::string + address type (0=public, 1=random)
  // Try public address first, then random — Veepeak uses random addressing
  NimBLEAddress addr(std::string(mac.c_str()), BLE_ADDR_RANDOM);
  Serial.print("BLE connecting to "); Serial.println(mac);

  if (!bleClient->connect(addr)) {
    Serial.println("BLE connect failed");
    return false;
  }

  // Find service FFF0
  NimBLERemoteService* svc = bleClient->getService(OBD_SERVICE_UUID);
  if (!svc) {
    // Try lowercase / full 128-bit equivalents
    auto& services = bleClient->getServices(true);
    for (auto* s : services) {
      std::string uuid = s->getUUID().toString();
      if (uuid.find("fff0") != std::string::npos) { svc = s; break; }
    }
  }
  if (!svc) {
    Serial.println("BLE: OBD service FFF0 not found");
    bleClient->disconnect();
    return false;
  }

  txChar = svc->getCharacteristic(OBD_WRITE_UUID);
  rxChar = svc->getCharacteristic(OBD_NOTIFY_UUID);

  if (!txChar || !rxChar) {
    Serial.println("BLE: TX/RX chars not found");
    bleClient->disconnect();
    return false;
  }

  if (rxChar->canNotify()) {
    rxChar->subscribe(true, notifyCallback);
  }

  connected = true;
  addSavedDevice(name, mac, 0);
  Serial.println("BLE connected to OBD adapter");
  return true;
}

// ── Async connect ────────────────────────────────────────────────
static volatile bool connectRunning = false;
static volatile bool connectDone    = false;
static volatile bool connectOk      = false;
static String        connectMac_g;
static String        connectName_g;

static void connectTask(void*) {
  connectOk      = connectToAddress(connectMac_g, connectName_g);
  connectRunning = false;
  connectDone    = true;
  vTaskDelete(nullptr);
}

void startConnectAsync(const String& mac, const String& name) {
  if (connectRunning) return;
  connectMac_g   = mac;
  connectName_g  = name;
  connectRunning = true;
  connectDone    = false;
  connectOk      = false;
  xTaskCreatePinnedToCore(connectTask, "BLEConn", 8192, nullptr, 1, nullptr, 0);
}

bool isConnectRunning() { return connectRunning; }

bool isConnectFinished(bool& success) {
  if (connectDone) { connectDone = false; success = connectOk; return true; }
  return false;
}

// ── Public connect helpers ───────────────────────────────────────
void connectToSavedDevice(int i, AppState& state) {
  BTSavedDevice* dev = getSavedDevice(i);
  if (!dev) { state = STATE_MENU_CONNECT; return; }
  if (!connectToAddress(dev->mac, dev->name)) state = STATE_MENU_CONNECT;
  else { resetOBD(); resetGaugePage(); state = STATE_INIT_ELM; }
}

void connectToSelectedDevice(AppState& state) {
  BTDeviceEntry* dev = getDevice(selectedIndex);
  if (!dev) { state = STATE_MENU_CONNECT; return; }
  if (!connectToAddress(dev->address, dev->name)) state = STATE_MENU_CONNECT;
  else { resetOBD(); resetGaugePage(); state = STATE_INIT_ELM; }
}

bool tryAutoConnect(AppState& state) {
  if (savedCount == 0) return false;
  BTSavedDevice* dev = getSavedDevice(defaultDeviceIdx);
  if (!dev || dev->mac == "") return false;
  return connectToAddress(dev->mac, dev->name);
}

// ── Health ───────────────────────────────────────────────────────
bool isBTConnected() { return bleConnected(); }

void disconnectBT() {
  if (bleClient && bleClient->isConnected()) bleClient->disconnect();
  connected = false; txChar = nullptr; rxChar = nullptr;
}

void handleBT(AppState& state) {
  if ((state == STATE_GAUGE || state == STATE_INIT_ELM) && !bleConnected() && connected) {
    Serial.println("BLE lost");
    connected = false;
    resetOBD();
  }
}

// ── Init ─────────────────────────────────────────────────────────
void initBT() {
  NimBLEDevice::init("OBD2Gauge");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  loadSavedDevices();
  Serial.println("BLE init done");
}
