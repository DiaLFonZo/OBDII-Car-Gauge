#include "debug_screen.h"
#include "ble.h"
#include "pids.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

// ── PID indices (must match STANDARD_PIDS order in pids.h) ───────
#define PID_IDX_RPM    0
#define PID_IDX_WATER  2    // WATER TEMP
#define PID_IDX_OIL    3    // OIL TEMP

// ── Widgets ──────────────────────────────────────────────────────
static lv_obj_t *s_scr        = NULL;
static lv_obj_t *s_status_lbl = NULL;
static lv_obj_t *s_rpm_val    = NULL;
static lv_obj_t *s_water_val  = NULL;
static lv_obj_t *s_oil_val    = NULL;

// ── Helpers ──────────────────────────────────────────────────────

static lv_obj_t* make_label(lv_obj_t *parent, const char *text,
                             const lv_font_t *font, lv_color_t color,
                             lv_align_t align, int x, int y)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, color, LV_PART_MAIN);
    lv_obj_align(lbl, align, x, y);
    return lbl;
}

// ── Timer callback — runs every 300ms ────────────────────────────

static void debug_tick(lv_timer_t *t) {
    (void)t;

    ble_app_state_t state = ble_get_state();
    obd_data_t *obd = obd_get_data();

    // Status line
    if (state == BLE_STATE_POLLING) {
        char buf[48];
        if (obd_lock(5)) {
            snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI "  %s", obd->device_name);
            obd_unlock();
        }
        lv_label_set_text(s_status_lbl, buf);
        lv_obj_set_style_text_color(s_status_lbl,
            lv_color_hex(0x00FF88), LV_PART_MAIN);
    } else if (state == BLE_STATE_SCANNING) {
        lv_label_set_text(s_status_lbl, LV_SYMBOL_REFRESH "  Scanning...");
        lv_obj_set_style_text_color(s_status_lbl,
            lv_color_hex(0x888888), LV_PART_MAIN);
    } else if (state == BLE_STATE_CONNECTING ||
               state == BLE_STATE_DISCOVERING ||
               state == BLE_STATE_INIT_ELM) {
        lv_label_set_text(s_status_lbl, LV_SYMBOL_REFRESH "  Connecting...");
        lv_obj_set_style_text_color(s_status_lbl,
            lv_color_hex(0x1E90FF), LV_PART_MAIN);
    } else {
        lv_label_set_text(s_status_lbl, LV_SYMBOL_CLOSE "  Disconnected");
        lv_obj_set_style_text_color(s_status_lbl,
            lv_color_hex(0xFF4444), LV_PART_MAIN);
    }

    // OBD values
    if (!obd_lock(5)) return;

    char rpm_str[16], water_str[16], oil_str[16];

    if (obd->has_data[PID_IDX_RPM])
        snprintf(rpm_str, sizeof(rpm_str), "%.0f", obd->values[PID_IDX_RPM]);
    else
        snprintf(rpm_str, sizeof(rpm_str), "---");

    if (obd->has_data[PID_IDX_WATER])
        snprintf(water_str, sizeof(water_str), "%.0f°", obd->values[PID_IDX_WATER]);
    else
        snprintf(water_str, sizeof(water_str), "---");

    if (obd->has_data[PID_IDX_OIL])
        snprintf(oil_str, sizeof(oil_str), "%.0f°", obd->values[PID_IDX_OIL]);
    else
        snprintf(oil_str, sizeof(oil_str), "---");

    obd_unlock();

    lv_label_set_text(s_rpm_val,   rpm_str);
    lv_label_set_text(s_water_val, water_str);
    lv_label_set_text(s_oil_val,   oil_str);
}

// ── Public ───────────────────────────────────────────────────────

void debug_screen_create(void) {
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, LV_PART_MAIN);

    // ── Status (top center) ──────────────────────────────────────
    s_status_lbl = make_label(s_scr, LV_SYMBOL_REFRESH "  Connecting...",
                              &lv_font_montserrat_16,
                              lv_color_hex(0x1E90FF),
                              LV_ALIGN_TOP_MID, 0, 70);

    // ── RPM (center, large) ───────────────────────────────────────
    make_label(s_scr, "RPM",
               &lv_font_montserrat_16,
               lv_color_hex(0x888888),
               LV_ALIGN_CENTER, 0, -70);

    s_rpm_val = make_label(s_scr, "---",
                           &lv_font_montserrat_16,
                           lv_color_white(),
                           LV_ALIGN_CENTER, 0, -40);
    lv_obj_set_style_text_font(s_rpm_val, &lv_font_montserrat_16, LV_PART_MAIN);

    // Divider line
    lv_obj_t *line = lv_obj_create(s_scr);
    lv_obj_set_size(line, 200, 2);
    lv_obj_align(line, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_border_width(line, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(line, 0, LV_PART_MAIN);

    // ── Water temp (bottom left) ──────────────────────────────────
    make_label(s_scr, "WATER",
               &lv_font_montserrat_16,
               lv_color_hex(0x4488FF),
               LV_ALIGN_CENTER, -80, 60);

    s_water_val = make_label(s_scr, "---",
                             &lv_font_montserrat_16,
                             lv_color_white(),
                             LV_ALIGN_CENTER, -80, 90);

    // ── Oil temp (bottom right) ───────────────────────────────────
    make_label(s_scr, "OIL",
               &lv_font_montserrat_16,
               lv_color_hex(0xFF8844),
               LV_ALIGN_CENTER, 80, 60);

    s_oil_val = make_label(s_scr, "---",
                           &lv_font_montserrat_16,
                           lv_color_white(),
                           LV_ALIGN_CENTER, 80, 90);

    lv_scr_load(s_scr);
    lv_timer_create(debug_tick, 300, NULL);
}
