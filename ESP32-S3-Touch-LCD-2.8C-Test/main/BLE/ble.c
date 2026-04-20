#include "ble.h"
#include "pids.h"

#include <string.h>
#include <stdio.h>
#include <strings.h>

#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "host/ble_store.h"
//#include "store/config/ble_store_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "BLE_OBD";

// ── Shared state ─────────────────────────────────────────────────
static obd_data_t        s_obd        = {0};
static SemaphoreHandle_t s_obd_mutex  = NULL;
static ble_app_state_t   s_state      = BLE_STATE_IDLE;

// ── Scan list ─────────────────────────────────────────────────────
static ble_scan_entry_t  s_scan_list[BLE_SCAN_MAX_DEVICES] = {0};
static int               s_scan_count = 0;
static SemaphoreHandle_t s_scan_mutex = NULL;

// ── Connection / GATT ────────────────────────────────────────────
static uint16_t s_conn_handle   = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_write_handle  = 0;
static uint16_t s_notify_handle = 0;
static uint8_t  s_own_addr_type = 0;

// ── Pending connect (set by UI, consumed by BLE task) ────────────
static volatile int s_connect_req = -1;   // index into s_scan_list, -1 = none
static ble_addr_t   s_connect_addr = {0};

// ── ELM327 poll state ────────────────────────────────────────────
static volatile bool s_prompt   = false;
static char          s_resp_buf[256] = {0};
static int           s_resp_len = 0;
static int           s_poll_idx = 0;
static bool          s_pid_sent = false;
static uint8_t       s_skip_ctr[MAX_PIDS] = {0};

// ── NVS ──────────────────────────────────────────────────────────
#define NVS_NS       "obd_ble"
#define NVS_KEY_ADDR "addr"
#define NVS_KEY_NAME "name"

// ─────────────────────────────────────────────────────────────────
// Public getters / setters
// ─────────────────────────────────────────────────────────────────

obd_data_t* obd_get_data(void)       { return &s_obd; }
ble_app_state_t ble_get_state(void)  { return s_state; }

bool obd_lock(uint32_t timeout_ms) {
    return xSemaphoreTake(s_obd_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
void obd_unlock(void) { xSemaphoreGive(s_obd_mutex); }

int ble_get_scan_count(void) {
    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    int c = s_scan_count;
    xSemaphoreGive(s_scan_mutex);
    return c;
}

ble_scan_entry_t ble_get_scan_entry(int index) {
    ble_scan_entry_t e = {0};
    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    if (index >= 0 && index < s_scan_count)
        e = s_scan_list[index];
    xSemaphoreGive(s_scan_mutex);
    return e;
}

void ble_connect_to_index(int index) {
    // Called from LVGL task — just set a flag, BLE task picks it up
    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    if (index >= 0 && index < s_scan_count) {
        memcpy(s_connect_addr.val, s_scan_list[index].addr, 6);
        s_connect_addr.type = s_scan_list[index].addr_type;
        s_connect_req = index;
        ESP_LOGI(TAG, "Connect request: %s", s_scan_list[index].name);
    }
    xSemaphoreGive(s_scan_mutex);
}

void ble_forget_device(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_KEY_ADDR);
        nvs_erase_key(h, NVS_KEY_NAME);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Saved device cleared");
    }
}

// ─────────────────────────────────────────────────────────────────
// NVS helpers
// ─────────────────────────────────────────────────────────────────

static bool nvs_load_addr(ble_addr_t *out) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = sizeof(ble_addr_t);
    bool ok = (nvs_get_blob(h, NVS_KEY_ADDR, out, &len) == ESP_OK);
    nvs_close(h);
    return ok;
}

static void nvs_save_device(const ble_addr_t *addr, const char *name) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY_ADDR, addr, sizeof(ble_addr_t));
    nvs_set_str(h, NVS_KEY_NAME, name ? name : "");
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Device saved: %s", name ? name : "unknown");
}

// ─────────────────────────────────────────────────────────────────
// Name match
// ─────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────
// ELM327 helpers
// ─────────────────────────────────────────────────────────────────

static void elm_send_cmd(const char *cmd) {
    if (!s_write_handle) return;
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%s\r", cmd);
    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, len);
    if (om) {
        s_resp_buf[0] = '\0';
        s_resp_len    = 0;
        s_prompt      = false;
        ble_gattc_write_no_rsp(s_conn_handle, s_write_handle, om);
    }
}

static bool elm_send_wait(const char *cmd, uint32_t timeout_ms) {
    elm_send_cmd(cmd);
    uint32_t t = 0;
    while (!s_prompt && t < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(10));
        t += 10;
    }
    return s_prompt;
}

static void elm_init_sequence(void) {
    ESP_LOGI(TAG, "--- ELM327 init ---");
    elm_send_wait("ATZ",   2000); ESP_LOGI(TAG, "ATZ:  %s", s_resp_buf);
    s_prompt = false;
    elm_send_wait("ATE0",  800);
    s_prompt = false;
    elm_send_wait("ATS0",  500);
    s_prompt = false;
    elm_send_wait("ATL0",  500);
    s_prompt = false;
    elm_send_wait("ATH0",  500);
    s_prompt = false;
    elm_send_wait("ATAL",  500);
    s_prompt = false;
    elm_send_wait("ATSP0", 800);
    s_prompt = false;
    ESP_LOGI(TAG, "--- ELM327 ready ---");
}

// ─────────────────────────────────────────────────────────────────
// PID parsing
// ─────────────────────────────────────────────────────────────────

static void parse_pid_response(int idx) {
    const PIDDef *p = &PIDS[idx];
    char r[128];
    strncpy(r, s_resp_buf, sizeof(r) - 1);
    r[sizeof(r)-1] = '\0';

    // uppercase
    for (int i = 0; r[i]; i++)
        if (r[i] >= 'a' && r[i] <= 'z') r[i] -= 32;

    if (strstr(r, "NODATA") || strstr(r, "ERROR") || strstr(r, "?")) return;

    char cmd_up[16];
    strncpy(cmd_up, p->cmd, sizeof(cmd_up)-1);
    cmd_up[sizeof(cmd_up)-1] = '\0';
    for (int i = 0; cmd_up[i]; i++)
        if (cmd_up[i] >= 'a' && cmd_up[i] <= 'z') cmd_up[i] -= 32;

    char header[16] = {0};
    if      (strncmp(cmd_up, "01", 2) == 0) snprintf(header, sizeof(header), "41%s", cmd_up+2);
    else if (strncmp(cmd_up, "22", 2) == 0) snprintf(header, sizeof(header), "62%s", cmd_up+2);
    else                                      strncpy(header, cmd_up, sizeof(header)-1);

    char *pos = strstr(r, header);
    if (!pos) return;
    char *data = pos + strlen(header);
    if (strlen(data) < 2) return;

    char tmp[3] = {data[0], data[1], 0};
    uint8_t A = (uint8_t)strtol(tmp, NULL, 16);
    uint8_t B = 0, C = 0;
    if (strlen(data) >= 4) { tmp[0]=data[2]; tmp[1]=data[3]; B=(uint8_t)strtol(tmp,NULL,16); }
    if (strlen(data) >= 6) { tmp[0]=data[4]; tmp[1]=data[5]; C=(uint8_t)strtol(tmp,NULL,16); }

    float val = 0.0f;
    const char *f = p->formula;
    if      (strcmp(f,"A")==0)               val = A;
    else if (strcmp(f,"A-40")==0)            val = A - 40.0f;
    else if (strcmp(f,"(A*256+B)/4")==0)     val = ((A*256)+B)/4.0f;
    else if (strcmp(f,"(A*256+B)/10-40")==0) val = ((A*256)+B)/10.0f - 40.0f;
    else if (strcmp(f,"(A*256+B)/20")==0)    val = ((A*256)+B)/20.0f;
    else if (strcmp(f,"(A*256+B)/1000")==0)  val = ((A*256)+B)/1000.0f;
    else if (strcmp(f,"(A/2)-64")==0)        val = A/2.0f - 64.0f;
    else if (strcmp(f,"(A*100)/255")==0)     val = (A*100.0f)/255.0f;
    else if (strcmp(f,"(100/255)*C")==0)     val = (100.0f/255.0f)*C;
    else if (strcmp(f,"B-2")==0)             val = (B!=2) ? 1.0f : 0.0f;
    else                                      val = A;

    if (obd_lock(5)) {
        s_obd.values[idx]   = val;
        s_obd.has_data[idx] = true;
        obd_unlock();
    }
    ESP_LOGD(TAG, "[%s] = %.2f %s", p->name, val, p->unit);
}

static void advance_poll_index(void) {
    if (PID_COUNT == 0) return;
    for (int n = 0; n < PID_COUNT; n++) {
        s_poll_idx = (s_poll_idx + 1) % PID_COUNT;
        if (s_skip_ctr[s_poll_idx] > 0) { s_skip_ctr[s_poll_idx]--; continue; }
        if (!pidEnabled[s_poll_idx]) continue;
        s_skip_ctr[s_poll_idx] = PIDS[s_poll_idx].skip;
        return;
    }
}

static void reset_obd_state(void) {
    s_write_handle  = 0;
    s_notify_handle = 0;
    s_poll_idx      = 0;
    s_pid_sent      = false;
    s_prompt        = false;
    s_resp_buf[0]   = '\0';
    s_resp_len      = 0;
    memset(s_skip_ctr, 0, sizeof(s_skip_ctr));
    if (obd_lock(50)) {
        memset(s_obd.values,   0, sizeof(s_obd.values));
        memset(s_obd.has_data, 0, sizeof(s_obd.has_data));
        s_obd.connected  = false;
        s_obd.elm_ready  = false;
        obd_unlock();
    }
}

// ─────────────────────────────────────────────────────────────────
// GATT notify callback — called from NimBLE host task
// ─────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────
// GATT discovery callbacks
// ─────────────────────────────────────────────────────────────────

static uint16_t s_svc_start = 0;
static uint16_t s_svc_end   = 0;

static int ble_on_chr_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                            const struct ble_gatt_chr *chr, void *arg)
{
    if (error && error->status == BLE_HS_EDONE) {
        ESP_LOGI(TAG, "Char discovery done. write=0x%04X notify=0x%04X",
                 s_write_handle, s_notify_handle);

        if (s_write_handle && s_notify_handle) {
            // Subscribe to notifications (write 0x0001 to CCCD = handle+1)
            uint16_t cccd_val = 1;
            struct os_mbuf *om = ble_hs_mbuf_from_flat(&cccd_val, 2);
            if (om) ble_gattc_write_no_rsp(conn_handle, s_notify_handle + 1, om);

            s_state = BLE_STATE_INIT_ELM;
        } else {
            ESP_LOGE(TAG, "Required characteristics not found — disconnecting");
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;
    }
    if (error && error->status != 0) return 0;
    if (!chr) return 0;

    uint16_t uuid16 = ble_uuid_u16(&chr->uuid.u);
    ESP_LOGI(TAG, "  chr UUID=0x%04X val_handle=0x%04X", uuid16, chr->val_handle);

    if (uuid16 == OBD_NOTIFY_UUID || uuid16 == OBD_NOTIFY_UUID_ALT)
        s_notify_handle = chr->val_handle;
    if (uuid16 == OBD_WRITE_UUID  || uuid16 == OBD_WRITE_UUID_ALT)
        s_write_handle  = chr->val_handle;
    return 0;
}

static int ble_on_svc_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                            const struct ble_gatt_svc *svc, void *arg)
{
    if (error && error->status == BLE_HS_EDONE) {
        if (s_svc_start == 0) {
            ESP_LOGE(TAG, "OBD service not found");
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;
    }
    if (error && error->status != 0) return 0;
    if (!svc) return 0;

    uint16_t uuid16 = ble_uuid_u16(&svc->uuid.u);
    ESP_LOGI(TAG, "Service UUID=0x%04X start=0x%04X end=0x%04X", uuid16,
             svc->start_handle, svc->end_handle);

    if ((uuid16 == OBD_SVC_UUID || uuid16 == OBD_SVC_UUID_ALT) && s_svc_start == 0) {
        s_svc_start = svc->start_handle;
        s_svc_end   = svc->end_handle;
        ble_gattc_disc_all_chrs(conn_handle, s_svc_start, s_svc_end,
                                ble_on_chr_disc, NULL);
    }
    return 0;
}

// ─────────────────────────────────────────────────────────────────
// GAP event callback
// ─────────────────────────────────────────────────────────────────

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg) {
    switch (event->type) {

    // ── Scan result ──────────────────────────────────────────────
    case BLE_GAP_EVENT_DISC: {
        struct ble_gap_disc_desc *disc = &event->disc;
        char name[32] = {0};

        // Extract device name from AD structures
        struct ble_hs_adv_fields fields;
        if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) == 0) {
            if (fields.name && fields.name_len > 0) {
                int l = fields.name_len < 31 ? fields.name_len : 31;
                memcpy(name, fields.name, l);
                name[l] = '\0';
            }
        }
        if (name[0] == '\0') return 0;  // skip unnamed devices

        // Add to scan list if not already present
        xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
        bool found = false;
        for (int i = 0; i < s_scan_count; i++) {
            if (memcmp(s_scan_list[i].addr, disc->addr.val, 6) == 0) {
                s_scan_list[i].rssi = disc->rssi;
                found = true;
                break;
            }
        }
        if (!found && s_scan_count < BLE_SCAN_MAX_DEVICES) {
            ble_scan_entry_t *e = &s_scan_list[s_scan_count];
            strncpy(e->name, name, 31);
            memcpy(e->addr, disc->addr.val, 6);
            e->addr_type = disc->addr.type;
            e->rssi      = disc->rssi;
            e->valid     = true;
            s_scan_count++;
            ESP_LOGI(TAG, "[SCAN] %s  rssi=%d", name, disc->rssi);
        }
        xSemaphoreGive(s_scan_mutex);
        break;
    }

    // ── Scan complete ────────────────────────────────────────────
    case BLE_GAP_EVENT_DISC_COMPLETE:
        ESP_LOGI(TAG, "Scan complete. Found %d devices", s_scan_count);
        break;

    // ── Connected ────────────────────────────────────────────────
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_state = BLE_STATE_DISCOVERING;
            ESP_LOGI(TAG, "Connected! handle=%d", s_conn_handle);

            // Discover services
            s_svc_start = 0;
            s_svc_end   = 0;
            ble_gattc_disc_all_svcs(s_conn_handle, ble_on_svc_disc, NULL);
        } else {
            ESP_LOGW(TAG, "Connect failed: %d", event->connect.status);
            s_state = BLE_STATE_SCANNING;
        }
        break;

    // ── Disconnected ─────────────────────────────────────────────
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "Disconnected. reason=%d", event->disconnect.reason);
        reset_obd_state();
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_state = BLE_STATE_DISCONNECTED;
        break;

    // ── Notification received ────────────────────────────────────
    case BLE_GAP_EVENT_NOTIFY_RX: {
        struct os_mbuf *om = event->notify_rx.om;
        uint16_t len = OS_MBUF_PKTLEN(om);
        if (len > (int)(sizeof(s_resp_buf) - s_resp_len - 1))
            len = sizeof(s_resp_buf) - s_resp_len - 1;
        os_mbuf_copydata(om, 0, len, s_resp_buf + s_resp_len);
        s_resp_len += len;
        s_resp_buf[s_resp_len] = '\0';

        // Check for ELM327 prompt '>'
        if (strchr(s_resp_buf, '>')) {
            s_prompt = true;
        }
        break;
    }

    default:
        break;
    }
    return 0;
}

// ─────────────────────────────────────────────────────────────────
// BLE host sync / reset callbacks
// ─────────────────────────────────────────────────────────────────

static void ble_on_reset(int reason) {
    ESP_LOGW(TAG, "NimBLE reset: %d", reason);
    s_state = BLE_STATE_IDLE;
}

static void ble_on_sync(void) {
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    ESP_LOGI(TAG, "NimBLE synced. addr_type=%d", s_own_addr_type);
    s_state = BLE_STATE_SCANNING;
}

// ─────────────────────────────────────────────────────────────────
// Scan helpers
// ─────────────────────────────────────────────────────────────────

static void start_scan(void) {
    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    s_scan_count = 0;
    memset(s_scan_list, 0, sizeof(s_scan_list));
    xSemaphoreGive(s_scan_mutex);

    struct ble_gap_disc_params p = {
        .passive      = 0,
        .itvl         = 0x0010,
        .window       = 0x0010,
        .filter_policy = BLE_HCI_SCAN_FILT_NO_WL,
        .limited      = 0,
        .filter_duplicates = 0,
    };
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &p,
                          ble_gap_event_cb, NULL);
    if (rc != 0) ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
    else         ESP_LOGI(TAG, "Scanning...");
}

static void connect_to_addr(const ble_addr_t *addr) {
    ble_gap_disc_cancel();  // stop scan before connecting

    struct ble_gap_conn_params cp = {
        .scan_itvl     = 0x0010,
        .scan_window   = 0x0010,
        .itvl_min      = BLE_GAP_INITIAL_CONN_ITVL_MIN,
        .itvl_max      = BLE_GAP_INITIAL_CONN_ITVL_MAX,
        .latency       = 0,
        .supervision_timeout = 0x0100,
        .min_ce_len    = 0,
        .max_ce_len    = 0x0100,
    };
    int rc = ble_gap_connect(s_own_addr_type, addr, 30000, &cp,
                             ble_gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_connect failed: %d", rc);
        s_state = BLE_STATE_SCANNING;
    } else {
        ESP_LOGI(TAG, "Connecting...");
        s_state = BLE_STATE_CONNECTING;
    }
}

// ─────────────────────────────────────────────────────────────────
// OBD state machine task  (Core 0)
// ─────────────────────────────────────────────────────────────────

static void obd_state_task(void *arg) {
    // Wait for NimBLE sync
    while (s_state == BLE_STATE_IDLE) vTaskDelay(pdMS_TO_TICKS(100));

    // Try to auto-connect from saved MAC
    ble_addr_t saved_addr = {0};
    bool has_saved = nvs_load_addr(&saved_addr);
    if (has_saved) {
        ESP_LOGI(TAG, "Saved device found — scanning to confirm presence...");
        start_scan();
        s_state = BLE_STATE_SCANNING;
    }

    while (1) {
        switch (s_state) {

        // ── Scanning ─────────────────────────────────────────────
        case BLE_STATE_SCANNING:
            if (!ble_gap_disc_active()) start_scan();

            // Auto-connect if saved device appears in scan list
            if (has_saved) {
                xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
                for (int i = 0; i < s_scan_count; i++) {
                    if (memcmp(s_scan_list[i].addr, saved_addr.val, 6) == 0) {
                        ESP_LOGI(TAG, "Saved device found: %s — connecting",
                                 s_scan_list[i].name);
                        s_connect_addr = saved_addr;
                        s_connect_req  = i;
                        break;
                    }
                }
                xSemaphoreGive(s_scan_mutex);
                has_saved = false;  // only auto-attempt once per boot
            }

            // UI-triggered connect request
            if (s_connect_req >= 0) {
                int req = s_connect_req;
                s_connect_req = -1;
                connect_to_addr(&s_connect_addr);

                // Save device for next boot
                xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
                if (req < s_scan_count)
                    nvs_save_device(&s_connect_addr, s_scan_list[req].name);
                xSemaphoreGive(s_scan_mutex);
            }
            break;

        // ── Connecting / Discovering — handled by GAP callbacks ──
        case BLE_STATE_CONNECTING:
        case BLE_STATE_DISCOVERING:
            break;

        // ── Init ELM327 ──────────────────────────────────────────
        case BLE_STATE_INIT_ELM:
            buildPIDList();
            s_poll_idx = 0;
            s_pid_sent = false;
            memset(s_skip_ctr, 0, sizeof(s_skip_ctr));
            elm_init_sequence();

            if (obd_lock(100)) {
                s_obd.connected = true;
                s_obd.elm_ready = true;
                xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
                if (s_connect_req >= 0 && s_connect_req < s_scan_count)
                    strncpy(s_obd.device_name,
                            s_scan_list[s_connect_req].name, 31);
                xSemaphoreGive(s_scan_mutex);
                obd_unlock();
            }
            s_state = BLE_STATE_POLLING;
            ESP_LOGI(TAG, "Polling started");
            break;

        // ── Polling ──────────────────────────────────────────────
        case BLE_STATE_POLLING:
            if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
                s_state = BLE_STATE_DISCONNECTED;
                break;
            }
            if (s_prompt) {
                s_prompt  = false;
                s_pid_sent = false;
                parse_pid_response(s_poll_idx);
                s_resp_buf[0] = '\0';
                s_resp_len    = 0;

                // Prioritise RPM (idx 0) on gauge page
                advance_poll_index();

                elm_send_cmd(PIDS[s_poll_idx].cmd);
                vTaskDelay(pdMS_TO_TICKS(20));
                s_pid_sent = true;
            } else if (!s_pid_sent) {
                elm_send_cmd(PIDS[s_poll_idx].cmd);
                s_pid_sent = true;
            }
            break;

        // ── Disconnected — restart scan ───────────────────────────
        case BLE_STATE_DISCONNECTED:
            ESP_LOGI(TAG, "Reconnecting — restarting scan");
            vTaskDelay(pdMS_TO_TICKS(1000));
            s_state = BLE_STATE_SCANNING;
            break;

        default:
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ─────────────────────────────────────────────────────────────────
// NimBLE host task (required by nimble_port_freertos_init)
// ─────────────────────────────────────────────────────────────────

static void ble_host_task(void *param) {
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();          // blocks until nimble_port_stop()
    nimble_port_freertos_deinit();
}

// ─────────────────────────────────────────────────────────────────
// Public init
// ─────────────────────────────────────────────────────────────────

void ble_obd_start(void) {
    s_obd_mutex  = xSemaphoreCreateMutex();
    s_scan_mutex = xSemaphoreCreateMutex();
    s_connect_req = -1;

    // ESP-IDF v5: HCI and controller init is handled automatically by nimble_port_init
    nimble_port_init();

    ble_hs_cfg.reset_cb       = ble_on_reset;
    ble_hs_cfg.sync_cb        = ble_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    //ble_store_config_init();

    // OBD state machine task on Core 0
    xTaskCreatePinnedToCore(obd_state_task, "obd_state", 4096, NULL, 5, NULL, 0);

    // NimBLE host task (must also be on Core 0)
    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "BLE OBD started");
}
