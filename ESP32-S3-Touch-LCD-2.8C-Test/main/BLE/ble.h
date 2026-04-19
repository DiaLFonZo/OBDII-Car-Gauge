#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// ── Target device names — first substring match wins ─────────────
#define BLE_TARGET_NAMES  { "VEEPEAK", "OBDCheck BLE", "IOS-Vlink", \
                             "ELM327", "OBD2", "Vlink", "OBDLink"  }
#define BLE_TARGET_COUNT  7

// ── GATT UUIDs (ELM327 standard + FFE0 fallback) ─────────────────
#define OBD_SVC_UUID         0xFFF0
#define OBD_NOTIFY_UUID      0xFFF1
#define OBD_WRITE_UUID       0xFFF2
#define OBD_SVC_UUID_ALT     0xFFE0
#define OBD_NOTIFY_UUID_ALT  0xFFE1
#define OBD_WRITE_UUID_ALT   0xFFE2

// ── Scan list ────────────────────────────────────────────────────
#define BLE_SCAN_MAX_DEVICES 15

typedef struct {
    char    name[32];
    uint8_t addr[6];
    uint8_t addr_type;
    int8_t  rssi;
    bool    valid;
} ble_scan_entry_t;

// ── App state machine ────────────────────────────────────────────
typedef enum {
    BLE_STATE_IDLE = 0,
    BLE_STATE_SCANNING,
    BLE_STATE_CONNECTING,
    BLE_STATE_DISCOVERING,
    BLE_STATE_INIT_ELM,
    BLE_STATE_POLLING,
    BLE_STATE_DISCONNECTED,
} ble_app_state_t;

// ── Shared OBD data (BLE task writes, LVGL task reads) ───────────
#define MAX_PIDS 64

typedef struct {
    float   values[MAX_PIDS];
    bool    has_data[MAX_PIDS];
    bool    connected;
    bool    elm_ready;
    char    device_name[32];
} obd_data_t;

// ── Public API ───────────────────────────────────────────────────
void             ble_obd_start(void);
ble_app_state_t  ble_get_state(void);
int              ble_get_scan_count(void);
ble_scan_entry_t ble_get_scan_entry(int index);
void             ble_connect_to_index(int index);   // called from UI on long press
void             ble_forget_device(void);           // clear saved MAC from NVS

obd_data_t*      obd_get_data(void);
bool             obd_lock(uint32_t timeout_ms);
void             obd_unlock(void);
