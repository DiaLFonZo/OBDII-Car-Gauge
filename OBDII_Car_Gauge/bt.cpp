#include "bt.h"
#include "obd.h"
#include "input.h"

BluetoothSerial BTSerial;

static BTDeviceEntry deviceList[MAX_DEVICES];
static int           deviceCount   = 0;
static int           selectedIndex = 0;
static bool          btConnected   = false;

// ─────────────────────────────
// Getters
// ─────────────────────────────
int            getDeviceCount()   { return deviceCount; }
int            getSelectedIndex() { return selectedIndex; }
BTDeviceEntry* getDevice(int i)   { return (i >= 0 && i < deviceCount) ? &deviceList[i] : nullptr; }
void           setSelectedIndex(int i) { selectedIndex = i; }
bool           isBTConnected()    { return btConnected && BTSerial.connected(); }

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

void disconnectBT() {
  BTSerial.disconnect();
  btConnected = false;
}

// ─────────────────────────────
// Init
// ─────────────────────────────
void initBT() {
  BTSerial.begin("OBD2Gauge", true);  // true = master mode
  BTSerial.setPin("1234", 4);
}

// ─────────────────────────────
// Scan — discover nearby Classic BT devices
// ─────────────────────────────
void startScan() {
  deviceCount   = 0;
  selectedIndex = 0;

  BTScanResults* results = BTSerial.discover(5000);  // 5 second scan
  if (results) {
    for (int i = 0; i < results->getCount() && deviceCount < MAX_DEVICES; i++) {
      BTAdvertisedDevice* dev = results->getDevice(i);
      deviceList[deviceCount].name    = dev->getName().c_str();
      deviceList[deviceCount].address = dev->getAddress().toString().c_str();
      if (deviceList[deviceCount].name == "")
        deviceList[deviceCount].name = deviceList[deviceCount].address;
      deviceCount++;
    }
  }
}

void resumeScan() {
  startScan();
}

// ─────────────────────────────
// Connect by MAC string
// ─────────────────────────────
static bool connectByAddress(const String& address, const String& name, AppState &state) {
  BTSerial.disconnect();
  btConnected = false;

  // Convert "00:1d:a5:00:12:92" to uint8_t[6]
  uint8_t mac[6] = {0};
  String s = address;
  for (int i = 0; i < 6; i++) {
    int idx = s.indexOf(':');
    String byteStr = (idx == -1) ? s : s.substring(0, idx);
    mac[i] = strtol(byteStr.c_str(), nullptr, 16);
    if (idx != -1) s = s.substring(idx + 1);
  }

  Serial.print("BT connecting to "); Serial.println(address);

  if (BTSerial.connect(mac)) {
    btConnected = true;
    resetOBD();
    resetGaugePage();
    state = STATE_INIT_ELM;

    // Save for auto-connect
    Preferences prefs;
    prefs.begin("obd", false);
    prefs.putString("mac",  address);
    prefs.putString("name", name);
    prefs.end();
    return true;
  }

  Serial.println("BT connect failed");
  state = STATE_MENU_CONNECT;  // stay on connect screen, user can retry
  return false;
}

bool tryAutoConnect(AppState &state) {
  Preferences prefs;
  prefs.begin("obd", true);
  String savedMac  = prefs.getString("mac",  "");
  String savedName = prefs.getString("name", "");
  prefs.end();

  if (savedMac == "") return false;

  Serial.print("Auto-connect to "); Serial.println(savedMac);
  return connectByAddress(savedMac, savedName, state);
}

void connectToSelectedDevice(AppState &state) {
  BTDeviceEntry* dev = getDevice(selectedIndex);
  if (!dev) { state = STATE_MENU_CONNECT; return; }
  connectByAddress(dev->address, dev->name, state);
}

// ─────────────────────────────
// handleBT — check connection health, drop cleanly if lost
// Never redirects to STATE_SCANNING — that state no longer exists.
// Caller stays in STATE_GAUGE with red dot; user reconnects via menu.
void handleBT(AppState &state) {
  if (state == STATE_GAUGE || state == STATE_INIT_ELM) {
    if (btConnected && !BTSerial.connected()) {
      Serial.println("BT disconnected");
      btConnected = false;
      resetOBD();
      state = STATE_GAUGE;  // stay in gauge, dot turns red
    }
  }
}
