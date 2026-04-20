#include "scan_screen.h"
#include "ble.h"
#include "gauge_screen.h"

#include "lvgl.h"
#include <string.h>
#include <stdio.h>

// ── Layout — all Y values are pixels from TOP of 480x480 screen ──
#define TITLE_Y         55
#define STATUS_Y        90
#define LIST_Y_START    115
#define LIST_ITEM_H     44
#define LIST_VISIBLE    4

#define BTN_ROW1_Y      340     // UP / DOWN buttons
#define BTN_ROW2_Y      395     // CONNECT button (in lower arc)
#define BTN_W           140
#define BTN_H           46
#define BTN_CONNECT_W   180

// ── State ────────────────────────────────────────────────────────
static lv_obj_t *s_scr        = NULL;
static lv_obj_t *s_status_lbl = NULL;
static lv_obj_t *s_rows[LIST_VISIBLE]       = {0};
static lv_obj_t *s_row_labels[LIST_VISIBLE] = {0};
static lv_obj_t *s_row_rssi[LIST_VISIBLE]   = {0};

static int  s_selected      = 0;
static int  s_scroll_top    = 0;
static bool s_transitioning = false;

// ─────────────────────────────────────────────────────────────────
// List refresh
// ─────────────────────────────────────────────────────────────────

static void refresh_list(void) {
    int total = ble_get_scan_count();

    if (s_selected < 0) s_selected = 0;
    if (total > 0 && s_selected >= total) s_selected = total - 1;

    if (s_selected < s_scroll_top)
        s_scroll_top = s_selected;
    if (s_selected >= s_scroll_top + LIST_VISIBLE)
        s_scroll_top = s_selected - LIST_VISIBLE + 1;

    for (int row = 0; row < LIST_VISIBLE; row++) {
        int idx = s_scroll_top + row;
        if (idx < total) {
            ble_scan_entry_t e = ble_get_scan_entry(idx);
            lv_label_set_text(s_row_labels[row], e.name);
            char rssi_str[12];
            snprintf(rssi_str, sizeof(rssi_str), "%d", e.rssi);
            lv_label_set_text(s_row_rssi[row], rssi_str);

            if (idx == s_selected) {
                lv_obj_set_style_bg_color(s_rows[row],
                    lv_color_hex(0xCC1111), LV_PART_MAIN);
                lv_obj_set_style_bg_opa(s_rows[row], LV_OPA_70, LV_PART_MAIN);
                lv_obj_set_style_text_color(s_row_labels[row],
                    lv_color_white(), LV_PART_MAIN);
                lv_obj_set_style_text_color(s_row_rssi[row],
                    lv_color_white(), LV_PART_MAIN);
            } else {
                lv_obj_set_style_bg_opa(s_rows[row], LV_OPA_0, LV_PART_MAIN);
                lv_obj_set_style_text_color(s_row_labels[row],
                    lv_color_hex(0xCCCCCC), LV_PART_MAIN);
                lv_obj_set_style_text_color(s_row_rssi[row],
                    lv_color_hex(0x888888), LV_PART_MAIN);
            }
            lv_obj_clear_flag(s_rows[row], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_rows[row], LV_OBJ_FLAG_HIDDEN);
        }
    }

    ble_app_state_t st = ble_get_state();
    if (st == BLE_STATE_CONNECTING || st == BLE_STATE_DISCOVERING) {
        lv_label_set_text(s_status_lbl, LV_SYMBOL_REFRESH "  Connecting...");
    } else if (st == BLE_STATE_INIT_ELM) {
        lv_label_set_text(s_status_lbl, LV_SYMBOL_REFRESH "  Initializing...");
    } else if (total == 0) {
        lv_label_set_text(s_status_lbl, LV_SYMBOL_REFRESH "  Scanning...");
    } else {
        char buf[40];
        snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI "  %d device%s found",
                 total, total == 1 ? "" : "s");
        lv_label_set_text(s_status_lbl, buf);
    }
}

// ─────────────────────────────────────────────────────────────────
// Callbacks
// ─────────────────────────────────────────────────────────────────

static void cb_up(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
        if (s_selected > 0) { s_selected--; refresh_list(); }
}

static void cb_dn(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        int total = ble_get_scan_count();
        if (s_selected < total - 1) { s_selected++; refresh_list(); }
    }
}

static void cb_connect(lv_event_t *e) {
    (void)e;
    if (s_transitioning) return;
    int total = ble_get_scan_count();
    if (total == 0 || s_selected >= total) return;
    s_transitioning = true;
    ble_connect_to_index(s_selected);
    lv_label_set_text(s_status_lbl, LV_SYMBOL_REFRESH "  Connecting...");
}

// ─────────────────────────────────────────────────────────────────
// Tick
// ─────────────────────────────────────────────────────────────────

static void scan_tick_cb(lv_timer_t *t) {
    ble_app_state_t st = ble_get_state();
    if (st == BLE_STATE_POLLING) {
        lv_timer_del(t);
        gauge_screen_show();
        return;
    }
    refresh_list();
}

// ─────────────────────────────────────────────────────────────────
// Public
// ─────────────────────────────────────────────────────────────────

void scan_screen_create(void) {
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, LV_PART_MAIN);

    // Title
    lv_obj_t *title = lv_label_create(s_scr);
    lv_label_set_text(title, "BLE SCAN");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, TITLE_Y);

    // Status
    s_status_lbl = lv_label_create(s_scr);
    lv_label_set_text(s_status_lbl, LV_SYMBOL_REFRESH "  Scanning...");
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0x1E90FF), LV_PART_MAIN);
    lv_obj_align(s_status_lbl, LV_ALIGN_TOP_MID, 0, STATUS_Y);

    // Device rows
    for (int i = 0; i < LIST_VISIBLE; i++) {
        int y = LIST_Y_START + i * LIST_ITEM_H;

        s_rows[i] = lv_obj_create(s_scr);
        lv_obj_set_size(s_rows[i], 320, LIST_ITEM_H - 4);
        lv_obj_align(s_rows[i], LV_ALIGN_TOP_MID, 0, y);
        lv_obj_set_style_border_width(s_rows[i], 0, LV_PART_MAIN);
        lv_obj_set_style_radius(s_rows[i], 8, LV_PART_MAIN);
        lv_obj_set_style_pad_all(s_rows[i], 4, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_rows[i], LV_OPA_0, LV_PART_MAIN);
        lv_obj_add_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN);

        s_row_labels[i] = lv_label_create(s_rows[i]);
        lv_obj_align(s_row_labels[i], LV_ALIGN_LEFT_MID, 8, 0);
        lv_obj_set_style_text_color(s_row_labels[i],
            lv_color_hex(0xCCCCCC), LV_PART_MAIN);
        lv_label_set_text(s_row_labels[i], "");

        s_row_rssi[i] = lv_label_create(s_rows[i]);
        lv_obj_align(s_row_rssi[i], LV_ALIGN_RIGHT_MID, -8, 0);
        lv_obj_set_style_text_color(s_row_rssi[i],
            lv_color_hex(0x888888), LV_PART_MAIN);
        lv_label_set_text(s_row_rssi[i], "");
    }

    // UP button
    lv_obj_t *btn_up = lv_btn_create(s_scr);
    lv_obj_set_size(btn_up, BTN_W, BTN_H);
    lv_obj_align(btn_up, LV_ALIGN_TOP_MID, -(BTN_W/2 + 8), BTN_ROW1_Y);
    lv_obj_set_style_bg_color(btn_up, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_radius(btn_up, 10, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_up, cb_up, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lup = lv_label_create(btn_up);
    lv_label_set_text(lup, LV_SYMBOL_UP "  UP");
    lv_obj_center(lup);

    // DOWN button
    lv_obj_t *btn_dn = lv_btn_create(s_scr);
    lv_obj_set_size(btn_dn, BTN_W, BTN_H);
    lv_obj_align(btn_dn, LV_ALIGN_TOP_MID, (BTN_W/2 + 8), BTN_ROW1_Y);
    lv_obj_set_style_bg_color(btn_dn, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_radius(btn_dn, 10, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_dn, cb_dn, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ldn = lv_label_create(btn_dn);
    lv_label_set_text(ldn, LV_SYMBOL_DOWN "  DOWN");
    lv_obj_center(ldn);

    // CONNECT button — centered below UP/DOWN in lower arc
    lv_obj_t *btn_conn = lv_btn_create(s_scr);
    lv_obj_set_size(btn_conn, BTN_CONNECT_W, BTN_H);
    lv_obj_align(btn_conn, LV_ALIGN_TOP_MID, 0, BTN_ROW2_Y);
    lv_obj_set_style_bg_color(btn_conn, lv_color_hex(0xCC1111), LV_PART_MAIN);
    lv_obj_set_style_radius(btn_conn, 10, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_conn, cb_connect, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lconn = lv_label_create(btn_conn);
    lv_label_set_text(lconn, LV_SYMBOL_BLUETOOTH "  CONNECT");
    lv_obj_center(lconn);

    lv_scr_load(s_scr);
    lv_timer_create(scan_tick_cb, 500, NULL);

    s_selected      = 0;
    s_scroll_top    = 0;
    s_transitioning = false;
}

void scan_screen_update(void) {
    refresh_list();
}