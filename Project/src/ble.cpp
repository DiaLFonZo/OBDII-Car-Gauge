#include "ble.h"
#include "obd.h"
#include "input.h"
#include <Preferences.h>


static BLEDeviceEntry deviceList[MAX_DEVICES];
static int selectedIndex = 0;
static int deviceCount   = 0;

static NimBLEScan*   scan   = nullptr;
static NimBLEClient* client = nullptr;

// ─────────────────────────────
// Getters
// ─────────────────────────────

int             getDeviceCount()    { return deviceCount; }
int             getSelectedIndex()  { return selectedIndex; }
NimBLEClient*   getBLEClient()      { return client; }

BLEDeviceEntry* getDevice(int index) {
  if (index < 0 || index >= deviceCount) return nullptr;
  return &deviceList[index];
}

void setSelectedIndex(int index) { selectedIndex = index; }

void nextDevice() {
  if (deviceCount == 0) return;
  selectedIndex = (selectedIndex + 1) % deviceCount;
}

String getSavedDeviceName() {
  Preferences prefs;
  prefs.begin("obd", true);
  String n = prefs.getString("name", "");
  prefs.end();
  return n;
}

void forgetSavedDevice() {
  Preferences prefs;
  prefs.begin("obd", false);
  prefs.remove("mac");
  prefs.remove("name");
  prefs.end();
}

// ─────────────────────────────
// Scan callback
// ─────────────────────────────

class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* device) override {
    String addr = device->getAddress().toString().c_str();
    uint8_t type = device->getAddress().getType();
    String name  = device->getName().c_str();

    for (int i = 0; i < deviceCount; i++) {
      if (deviceList[i].address == addr) {
        if (name != "") deviceList[i].name = name;
        return;  // already known — no print
      }
    }
    if (deviceCount >= MAX_DEVICES) return;
    if (name == "") name = addr.substring(0, 8);
    // only print truly new devices
    Serial.printf("[SCAN] NEW: '%s'  %s  type=%d  rssi=%d\n",
                  name.c_str(), addr.c_str(), type, device->getRSSI());
    deviceList[deviceCount].name     = name;
    deviceList[deviceCount].address  = addr;
    deviceList[deviceCount].addrType = type;
    deviceCount++;
  }
};

// ─────────────────────────────
// Init
// ─────────────────────────────

void initBLE() {
  Serial.println("[BLE] initBLE()");
  NimBLEDevice::init("OBD2");
  scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  scan->setAdvertisedDeviceCallbacks(new ScanCallbacks());
  Serial.println("[BLE] initBLE() done");
}

static void scanComplete(NimBLEScanResults) {}  // non-blocking requires a callback

void startScan() {
  Serial.println("[BLE] startScan()");
  deviceCount   = 0;
  selectedIndex = 0;
  scan->clearResults();
  scan->start(0, scanComplete, false);  // 3-arg version returns immediately
}

void stopScan() {
  scan->stop();
}

void resumeScan() {
  scan->start(0, scanComplete, true);  // non-blocking resume
}

// ─────────────────────────────
// Connect by address string
// ─────────────────────────────

static bool connectByAddress(const String& address, uint8_t addrType, AppState &state) {
  Serial.printf("[BLE] connectByAddress: %s  addrType=%d\n", address.c_str(), addrType);

  if (client) {
    Serial.println("[BLE] Deleting old client");
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    client = nullptr;
  }

  Serial.println("[BLE] Creating client...");
  client = NimBLEDevice::createClient();
  if (!client) {
    Serial.println("[BLE] ERR: createClient() returned null");
    resumeScan();
    state = STATE_SCANNING;
    return false;
  }
  client->setConnectTimeout(10);
  Serial.println("[BLE] Client created. Calling connect()...");

  NimBLEAddress bleAddr(std::string(address.c_str()), addrType);
  bool connected = client->connect(bleAddr);
  Serial.printf("[BLE] connect() returned: %s\n", connected ? "TRUE" : "FALSE");

  if (!connected) {
    Serial.printf("[BLE] connect() failed. isConnected=%d\n", client->isConnected());
    NimBLEDevice::deleteClient(client);
    client = nullptr;
    resumeScan();
    state = STATE_SCANNING;
    return false;
  }

  Serial.println("[BLE] Connected! Waiting for stack to settle...");
  delay(500);  // give GATT stack time before service discovery
  resetOBD();
  resetGaugePage();
  state = STATE_INIT_ELM;
  return true;
}

bool tryAutoConnect(AppState &state) {
  Preferences prefs;
  prefs.begin("obd", true);
  String savedMac = prefs.getString("mac", "");
  prefs.end();
  if (savedMac == "") return false;

  for (int i = 0; i < deviceCount; i++) {
    if (deviceList[i].address == savedMac) {
      selectedIndex = i;
      connectByAddress(savedMac, deviceList[i].addrType, state);
      return true;
    }
  }
  return false;
}

void connectToSelectedDevice(AppState &state) {
  Serial.printf("[BLE] connectToSelectedDevice: count=%d idx=%d\n", deviceCount, selectedIndex);
  if (deviceCount == 0 || selectedIndex >= deviceCount) {
    Serial.println("[BLE] ERR: no device to connect to");
    state = STATE_SCANNING;
    return;
  }
  BLEDeviceEntry* dev = &deviceList[selectedIndex];
  Serial.printf("[BLE] Target: name='%s' addr='%s' type=%d\n",
                dev->name.c_str(), dev->address.c_str(), dev->addrType);

  bool ok = connectByAddress(dev->address, dev->addrType, state);
  if (ok) {
    Preferences prefs;
    prefs.begin("obd", false);
    prefs.putString("mac",  dev->address);
    prefs.putString("name", dev->name);
    prefs.end();
  }
}

void handleBLE(AppState &state) {
  if (state != STATE_CONNECTING) return;
  static unsigned long connectStart = 0;
  if (connectStart == 0) connectStart = millis();

  if (client && client->isConnected()) {
    connectStart = 0;
    resetOBD();
    state = STATE_INIT_ELM;
    return;
  }
  if (millis() - connectStart > 6000) {
    if (client) {
      client->disconnect();
      NimBLEDevice::deleteClient(client);
      client = nullptr;
    }
    connectStart = 0;
    resumeScan();
    state = STATE_SCANNING;
  }
}