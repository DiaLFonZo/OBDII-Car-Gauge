#include "gauge_screen.h"
#include "gauge_config.h"
#include "ble.h"
#include "pids.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "lvgl.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "GAUGE";

// ── Font declarations ─────────────────────────────────────────────
LV_FONT_DECLARE(font_dseg7_48);
LV_FONT_DECLARE(font_dseg7_55);
LV_FONT_DECLARE(font_dseg7_65);
LV_FONT_DECLARE(font_dseg7_75);

// ── BMP background ────────────────────────────────────────────────
#define BMP_PATH      "/sdcard/gauge_2.bmp"
#define BMP_W         480
#define BMP_H         480

static uint16_t    *s_bg_buf    = NULL;
static lv_img_dsc_t s_bg_dsc   = {0};
static bool         s_bg_loaded = false;

// ── State ─────────────────────────────────────────────────────────
static int       s_page         = 0;
static lv_obj_t *s_scr          = NULL;

// Arc page widgets
static lv_obj_t              *s_meter       = NULL;
static lv_meter_indicator_t  *s_needle      = NULL;
static lv_obj_t              *s_digit_lbl   = NULL;
static lv_obj_t              *s_status_lbl  = NULL;
static int                    s_last_val    = -9999;

// Dual page widgets
static lv_obj_t *s_dual_bar_l   = NULL;
static lv_obj_t *s_dual_bar_r   = NULL;
static lv_obj_t *s_dual_val_l   = NULL;
static lv_obj_t *s_dual_val_r   = NULL;
static lv_obj_t *s_dual_name_l  = NULL;
static lv_obj_t *s_dual_name_r  = NULL;

// Raw page widgets
static lv_obj_t *s_raw_labels[MAX_PIDS] = {NULL};

// ─────────────────────────────────────────────────────────────────
// Font picker
// ─────────────────────────────────────────────────────────────────

static const lv_font_t* dseg7_font(int size) {
    if (size <= 48) return &font_dseg7_48;
    if (size <= 55) return &font_dseg7_55;
    if (size <= 65) return &font_dseg7_65;
    return &font_dseg7_75;
}

static const lv_font_t* mont_font(int size) {
    if (size <= 14) return &lv_font_montserrat_14;
    if (size <= 16) return &lv_font_montserrat_16;
    if (size <= 20) return &lv_font_montserrat_16;
    return &lv_font_montserrat_16;
}

static lv_color_t hex(uint32_t c) {
    return lv_color_make((c>>16)&0xFF, (c>>8)&0xFF, c&0xFF);
}

// ─────────────────────────────────────────────────────────────────
// BMP loader — reads /sdcard/gauge_2.bmp into PSRAM
// ─────────────────────────────────────────────────────────────────

void gauge_screen_init(void) {
    FILE *f = fopen(BMP_PATH, "rb");
    if (!f) {
        ESP_LOGW(TAG, "BMP not found: %s", BMP_PATH);
        return;
    }

    uint8_t hdr[54];
    if (fread(hdr, 1, 54, f) != 54 || hdr[0] != 'B' || hdr[1] != 'M') {
        ESP_LOGE(TAG, "Invalid BMP header");
        fclose(f); return;
    }

    uint32_t offset = hdr[10] | (hdr[11]<<8) | (hdr[12]<<16) | (hdr[13]<<24);
    int32_t  w      = (int32_t)(hdr[18]|(hdr[19]<<8)|(hdr[20]<<16)|(hdr[21]<<24));
    int32_t  h      = (int32_t)(hdr[22]|(hdr[23]<<8)|(hdr[24]<<16)|(hdr[25]<<24));
    uint16_t bpp    = hdr[28] | (hdr[29]<<8);

    if (w != BMP_W || abs(h) != BMP_H || (bpp != 24 && bpp != 32)) {
        ESP_LOGE(TAG, "BMP must be %dx%d 24/32bpp, got %dx%d %dbpp",
                 BMP_W, BMP_H, w, abs(h), bpp);
        fclose(f); return;
    }

    if (!s_bg_buf)
        s_bg_buf = (uint16_t*)heap_caps_malloc(BMP_W * BMP_H * 2, MALLOC_CAP_SPIRAM);
    if (!s_bg_buf) {
        ESP_LOGE(TAG, "PSRAM alloc failed");
        fclose(f); return;
    }

    fseek(f, offset, SEEK_SET);
    int     Bpp    = bpp / 8;
    int     stride = ((w * Bpp + 3) / 4) * 4;
    uint8_t *row   = (uint8_t*)malloc(stride);
    if (!row) { fclose(f); return; }

    for (int r = 0; r < BMP_H; r++) {
        fread(row, 1, stride, f);
        int dst_row = (h > 0) ? (BMP_H - 1 - r) : r;
        uint16_t *dst = s_bg_buf + dst_row * BMP_W;
        for (int c = 0; c < BMP_W; c++) {
            uint8_t b = row[c*Bpp];
            uint8_t g = row[c*Bpp+1];
            uint8_t rv = row[c*Bpp+2];
            dst[c] = ((uint16_t)(rv & 0xF8) << 8) |
                     ((uint16_t)(g  & 0xFC) << 3) |
                     (b >> 3);
        }
    }
    free(row);
    fclose(f);

    s_bg_dsc.header.cf          = LV_IMG_CF_TRUE_COLOR;
    s_bg_dsc.header.always_zero = 0;
    s_bg_dsc.header.reserved    = 0;
    s_bg_dsc.header.w           = BMP_W;
    s_bg_dsc.header.h           = BMP_H;
    s_bg_dsc.data_size          = BMP_W * BMP_H * 2;
    s_bg_dsc.data               = (const uint8_t*)s_bg_buf;
    s_bg_loaded = true;
    ESP_LOGI(TAG, "BMP loaded OK");
}

// ─────────────────────────────────────────────────────────────────
// Page builders
// ─────────────────────────────────────────────────────────────────

static void build_arc_page(const arc_cfg_t *c) {
    s_meter     = NULL;
    s_needle    = NULL;
    s_digit_lbl = NULL;
    s_last_val  = -9999;

    lv_obj_t *scr = s_scr;

    // Background image
    if (s_bg_loaded) {
        lv_obj_t *bg = lv_img_create(scr);
        lv_img_set_src(bg, &s_bg_dsc);
        lv_obj_set_pos(bg, 0, 0);
    } else {
        lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
        lv_obj_t *err = lv_label_create(scr);
        lv_label_set_text(err, "gauge_2.bmp not found");
        lv_obj_set_style_text_color(err, lv_color_hex(0xFF6600), LV_PART_MAIN);
        lv_obj_align(err, LV_ALIGN_TOP_MID, 0, 15);
    }

    // Meter
    s_meter = lv_meter_create(scr);
    lv_obj_set_size(s_meter, 420, 420);
    lv_obj_align(s_meter, LV_ALIGN_CENTER, 0, 5);
    lv_obj_set_style_bg_opa(s_meter,     LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_opa(s_meter, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_meter, 10, LV_PART_MAIN);

    lv_meter_scale_t *scale = lv_meter_add_scale(s_meter);
    lv_meter_set_scale_range(s_meter, scale,
                             c->val_min, c->val_max,
                             c->angle_range, c->angle_start);
    lv_meter_set_scale_ticks(s_meter, scale,
                             c->minor_count,
                             c->minor_width, c->minor_len,
                             hex(c->minor_color));
    lv_meter_set_scale_major_ticks(s_meter, scale,
                                   c->major_nth,
                                   c->major_width, c->major_len,
                                   hex(c->major_color), 8);

    // Hide numeric labels if not wanted
    if (!c->show_labels)
        lv_obj_set_style_text_opa(s_meter, LV_OPA_TRANSP, LV_PART_TICKS);

    // Hidden arc for scale definition (width 0)
    lv_meter_indicator_t *arc_n = lv_meter_add_arc(s_meter, scale, 0,
                                                    lv_color_black(), 0);
    lv_meter_set_indicator_start_value(s_meter, arc_n, c->val_min);
    lv_meter_set_indicator_end_value(s_meter, arc_n, c->warn_start > 0 ?
                                     c->warn_start : c->val_max);

    // Warn arc
    if (c->warn_start > 0) {
        lv_meter_indicator_t *arc_w = lv_meter_add_arc(s_meter, scale,
                                                        c->warn_arc_width,
                                                        hex(c->warn_color), -8);
        lv_meter_set_indicator_start_value(s_meter, arc_w, c->warn_start);
        lv_meter_set_indicator_end_value(s_meter, arc_w, c->warn_end);
    }

    // Needle
    s_needle = lv_meter_add_needle_line(s_meter, scale,
                                        c->needle_width,
                                        hex(c->needle_color),
                                        c->needle_tail);
    lv_meter_set_indicator_value(s_meter, s_needle, c->val_min);

    // Digital readout
    s_digit_lbl = lv_label_create(scr);
    lv_label_set_text(s_digit_lbl, "0000");
    lv_obj_set_style_text_font(s_digit_lbl, dseg7_font(c->font_size), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_digit_lbl, hex(c->digit_color), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_digit_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(s_digit_lbl, 240);
    lv_obj_align(s_digit_lbl, LV_ALIGN_CENTER, 0, c->digit_y);

    // Unit label
    if (c->unit_label) {
        lv_obj_t *unit = lv_label_create(scr);
        lv_label_set_text(unit, c->unit_label);
        lv_obj_set_style_text_font(unit, mont_font(c->unit_font_size), LV_PART_MAIN);
        lv_obj_set_style_text_color(unit, hex(c->unit_color), LV_PART_MAIN);
        lv_obj_align(unit, LV_ALIGN_CENTER, 0, c->unit_y);
    }

    // Status dot (top center — connection state)
    s_status_lbl = lv_label_create(scr);
    lv_label_set_text(s_status_lbl, "");
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(s_status_lbl, LV_ALIGN_TOP_MID, 0, 18);
}

static void build_dual_page(const dual_cfg_t *c) {
    s_dual_bar_l  = NULL; s_dual_bar_r  = NULL;
    s_dual_val_l  = NULL; s_dual_val_r  = NULL;
    s_dual_name_l = NULL; s_dual_name_r = NULL;

    lv_obj_t *scr = s_scr;
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);

    const PIDDef *pl = &PIDS[c->pid_left];
    const PIDDef *pr = &PIDS[c->pid_right];

    // Left value
    s_dual_name_l = lv_label_create(scr);
    lv_label_set_text(s_dual_name_l, pl->name);
    lv_obj_set_style_text_font(s_dual_name_l, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_dual_name_l, hex(c->color_left), LV_PART_MAIN);
    lv_obj_align(s_dual_name_l, LV_ALIGN_CENTER, -90, -80);

    s_dual_val_l = lv_label_create(scr);
    lv_label_set_text(s_dual_val_l, "---");
    lv_obj_set_style_text_font(s_dual_val_l, dseg7_font(c->font_size), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_dual_val_l, hex(c->color_left), LV_PART_MAIN);
    lv_obj_align(s_dual_val_l, LV_ALIGN_CENTER, -90, -20);

    lv_obj_t *unit_l = lv_label_create(scr);
    lv_label_set_text(unit_l, pl->unit);
    lv_obj_set_style_text_color(unit_l, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_align(unit_l, LV_ALIGN_CENTER, -90, 50);

    // Left bar
    s_dual_bar_l = lv_bar_create(scr);
    lv_obj_set_size(s_dual_bar_l, 20, 160);
    lv_bar_set_range(s_dual_bar_l, (int)pl->valMin, (int)pl->valMax);
    lv_bar_set_value(s_dual_bar_l, (int)pl->valMin, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_dual_bar_l, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_dual_bar_l, hex(c->color_left), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_dual_bar_l, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(s_dual_bar_l, 4, LV_PART_INDICATOR);
    lv_obj_align(s_dual_bar_l, LV_ALIGN_CENTER, -140, 0);

    // Right value
    s_dual_name_r = lv_label_create(scr);
    lv_label_set_text(s_dual_name_r, pr->name);
    lv_obj_set_style_text_font(s_dual_name_r, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_dual_name_r, hex(c->color_right), LV_PART_MAIN);
    lv_obj_align(s_dual_name_r, LV_ALIGN_CENTER, 90, -80);

    s_dual_val_r = lv_label_create(scr);
    lv_label_set_text(s_dual_val_r, "---");
    lv_obj_set_style_text_font(s_dual_val_r, dseg7_font(c->font_size), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_dual_val_r, hex(c->color_right), LV_PART_MAIN);
    lv_obj_align(s_dual_val_r, LV_ALIGN_CENTER, 90, -20);

    lv_obj_t *unit_r = lv_label_create(scr);
    lv_label_set_text(unit_r, pr->unit);
    lv_obj_set_style_text_color(unit_r, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_align(unit_r, LV_ALIGN_CENTER, 90, 50);

    // Right bar
    s_dual_bar_r = lv_bar_create(scr);
    lv_obj_set_size(s_dual_bar_r, 20, 160);
    lv_bar_set_range(s_dual_bar_r, (int)pr->valMin, (int)pr->valMax);
    lv_bar_set_value(s_dual_bar_r, (int)pr->valMin, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_dual_bar_r, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_dual_bar_r, hex(c->color_right), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_dual_bar_r, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(s_dual_bar_r, 4, LV_PART_INDICATOR);
    lv_obj_align(s_dual_bar_r, LV_ALIGN_CENTER, 140, 0);

    // Divider
    lv_obj_t *div = lv_obj_create(scr);
    lv_obj_set_size(div, 2, 200);
    lv_obj_align(div, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(div, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_border_width(div, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(div, 0, LV_PART_MAIN);

    // Page indicator
    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "Hold: next page");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -20);
}

static void build_raw_page(void) {
    memset(s_raw_labels, 0, sizeof(s_raw_labels));
    lv_obj_t *scr = s_scr;
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "LIVE OBD2 VALUES");
    lv_obj_set_style_text_color(title, lv_color_hex(0xAAAAAA), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 52);

    lv_obj_t *cont = lv_obj_create(scr);
    lv_obj_set_size(cont, 360, 340);
    lv_obj_align(cont, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(cont, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(cont, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(cont, LV_DIR_VER);

    for (int i = 0; i < PID_COUNT; i++) {
        lv_obj_t *row = lv_obj_create(cont);
        lv_obj_set_size(row, 352, 26);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, PIDS[i].name);
        lv_obj_set_style_text_color(name, lv_color_hex(0x999999), LV_PART_MAIN);
        lv_obj_set_width(name, 160);

        s_raw_labels[i] = lv_label_create(row);
        lv_label_set_text(s_raw_labels[i], "---");
        lv_obj_set_style_text_color(s_raw_labels[i], lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_align(s_raw_labels[i], LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
        lv_obj_set_width(s_raw_labels[i], 120);

        lv_obj_t *unit = lv_label_create(row);
        lv_label_set_text(unit, PIDS[i].unit);
        lv_obj_set_style_text_color(unit, lv_color_hex(0x666666), LV_PART_MAIN);
        lv_obj_set_width(unit, 60);
    }

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "Hold: next page");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);
}

// ─────────────────────────────────────────────────────────────────
// Long press → next page
// ─────────────────────────────────────────────────────────────────

static void on_long_press(lv_event_t *e) {
    (void)e;
    s_page = (s_page + 1) % GAUGE_PAGE_COUNT;

    // Rebuild screen
    lv_obj_clean(s_scr);

    const gauge_page_def_t *p = &GAUGE_PAGES[s_page];
    switch (p->type) {
        case GAUGE_TYPE_ARC:
            build_arc_page(&p->arc);
            break;
        case GAUGE_TYPE_DUAL:
            build_dual_page(&p->dual);
            break;
        case GAUGE_TYPE_RAW:
            build_raw_page();
            break;
        default: break;
    }

    // Re-attach long press to new screen
    lv_obj_add_flag(s_scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_scr, on_long_press, LV_EVENT_LONG_PRESSED, NULL);
}

// ─────────────────────────────────────────────────────────────────
// Update timer — runs every 100ms
// ─────────────────────────────────────────────────────────────────

static void gauge_tick(lv_timer_t *t) {
    (void)t;

    obd_data_t *obd = obd_get_data();
    if (!obd_lock(5)) return;

    const gauge_page_def_t *p = &GAUGE_PAGES[s_page];

    switch (p->type) {

    // ── Arc page ─────────────────────────────────────────────────
    case GAUGE_TYPE_ARC: {
        if (!s_meter || !s_needle || !s_digit_lbl) break;
        const arc_cfg_t *c = &p->arc;

        // Status
        if (s_status_lbl) {
            ble_app_state_t st = ble_get_state();
            if (st == BLE_STATE_POLLING)
                lv_label_set_text(s_status_lbl, "");
            else if (st == BLE_STATE_DISCONNECTED)
                lv_label_set_text(s_status_lbl, LV_SYMBOL_CLOSE " BLE Lost");
            else
                lv_label_set_text(s_status_lbl, LV_SYMBOL_REFRESH " ...");
        }

        if (!obd->has_data[0]) break;
        int val = (int)obd->values[0];
        val = val < c->val_min ? c->val_min : (val > c->val_max ? c->val_max : val);
        if (val == s_last_val) break;
        s_last_val = val;

        lv_meter_set_indicator_value(s_meter, s_needle, val);

        char buf[8];
        snprintf(buf, sizeof(buf), "%04d", val);
        lv_label_set_text(s_digit_lbl, buf);
        break;
    }

    // ── Dual page ────────────────────────────────────────────────
    case GAUGE_TYPE_DUAL: {
        if (!s_dual_val_l || !s_dual_val_r) break;
        const dual_cfg_t *c = &p->dual;

        // Left PID
        if (obd->has_data[c->pid_left]) {
            float v = obd->values[c->pid_left];
            char buf[12];
            snprintf(buf, sizeof(buf), "%.0f", v);
            lv_label_set_text(s_dual_val_l, buf);
            bool warn = PIDS[c->pid_left].warn > 0 && v >= PIDS[c->pid_left].warn;
            lv_obj_set_style_text_color(s_dual_val_l,
                warn ? hex(c->warn_color) : hex(c->color_left), LV_PART_MAIN);
            if (s_dual_bar_l)
                lv_bar_set_value(s_dual_bar_l, (int)v, LV_ANIM_ON);
        }

        // Right PID
        if (obd->has_data[c->pid_right]) {
            float v = obd->values[c->pid_right];
            char buf[12];
            snprintf(buf, sizeof(buf), "%.0f", v);
            lv_label_set_text(s_dual_val_r, buf);
            bool warn = PIDS[c->pid_right].warn > 0 && v >= PIDS[c->pid_right].warn;
            lv_obj_set_style_text_color(s_dual_val_r,
                warn ? hex(c->warn_color) : hex(c->color_right), LV_PART_MAIN);
            if (s_dual_bar_r)
                lv_bar_set_value(s_dual_bar_r, (int)v, LV_ANIM_ON);
        }
        break;
    }

    // ── Raw page ─────────────────────────────────────────────────
    case GAUGE_TYPE_RAW: {
        for (int i = 0; i < PID_COUNT; i++) {
            if (!s_raw_labels[i] || !obd->has_data[i]) continue;
            char buf[20];
            if (PIDS[i].isBoolean)
                snprintf(buf, sizeof(buf), "%s", obd->values[i] > 0.5f ? "ON" : "OFF");
            else
                snprintf(buf, sizeof(buf), "%.1f", obd->values[i]);
            lv_label_set_text(s_raw_labels[i], buf);
            bool warn = PIDS[i].warn > 0 && obd->values[i] >= PIDS[i].warn;
            lv_obj_set_style_text_color(s_raw_labels[i],
                warn ? lv_color_hex(0xFF3333) : lv_color_white(), LV_PART_MAIN);
        }
        break;
    }

    default: break;
    }

    obd_unlock();
}

// ─────────────────────────────────────────────────────────────────
// Public
// ─────────────────────────────────────────────────────────────────

void gauge_screen_show(void) {
    s_page = 0;
    s_scr  = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, LV_PART_MAIN);

    const gauge_page_def_t *p = &GAUGE_PAGES[0];
    switch (p->type) {
        case GAUGE_TYPE_ARC:  build_arc_page(&p->arc);   break;
        case GAUGE_TYPE_DUAL: build_dual_page(&p->dual); break;
        case GAUGE_TYPE_RAW:  build_raw_page();           break;
        default: break;
    }

    lv_obj_add_flag(s_scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_scr, on_long_press, LV_EVENT_LONG_PRESSED, NULL);

    lv_scr_load(s_scr);
    lv_timer_create(gauge_tick, 50, NULL);
    ESP_LOGI(TAG, "Gauge screen running");
}
