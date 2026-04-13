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
    for (int i = 0; i < deviceCount; i++) {
      if (deviceList[i].address == addr) {
        String n = device->getName().c_str();
        if (n != "") deviceList[i].name = n;
        return;
      }
    }
    if (deviceCount >= MAX_DEVICES) return;
    String name = device->getName().c_str();
    if (name == "") name = addr.substring(0, 8);
    deviceList[deviceCount].name    = name;
    deviceList[deviceCount].address = addr;
    deviceCount++;
  }
};

// ─────────────────────────────
// Init
// ─────────────────────────────

void initBLE() {
  NimBLEDevice::init("OBD2");
  scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  scan->setAdvertisedDeviceCallbacks(new ScanCallbacks());
}

void startScan() {
  deviceCount   = 0;
  selectedIndex = 0;
  scan->clearResults();
  scan->start(0, false);  // 0 = indefinite, truly non-blocking
}

void stopScan() {
  scan->stop();
}

void resumeScan() {
  scan->start(0, true);
}

// ─────────────────────────────
// Connect by address string
// ─────────────────────────────

static bool connectByAddress(const String& address, AppState &state) {
  if (client) {
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    client = nullptr;
  }
  scan->stop();
  delay(100);

  NimBLEAddress bleAddr(std::string(address.c_str()), BLE_ADDR_PUBLIC);
  client = NimBLEDevice::createClient();

  client->setConnectTimeout(10);
  if (!client->connect(bleAddr)) {
    NimBLEDevice::deleteClient(client);
    client = nullptr;
    resumeScan();
    state = STATE_SCANNING;
    return false;
  }

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
      connectByAddress(savedMac, state);
      return true;
    }
  }
  return false;
}

void connectToSelectedDevice(AppState &state) {
  if (deviceCount == 0) return;
  BLEDeviceEntry* dev = &deviceList[selectedIndex];
  if (!dev) return;

  bool ok = connectByAddress(dev->address, state);
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