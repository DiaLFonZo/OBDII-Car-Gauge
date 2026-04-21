#include "boot_screen.h"
#include "scan_screen.h"
#include "ble.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "ST7701S.h"
#include <stdio.h>

static const char *TAG = "BOOT";

#define BMP_PATH "/sdcard/gauge_2.bmp"
#define BMP_W    480
#define BMP_H    480

// ── BMP ───────────────────────────────────────────────────────────
static uint16_t    *s_bg_buf    = NULL;
static lv_img_dsc_t s_bg_dsc   = {0};
static bool         s_bg_loaded = false;

// ── Widgets ───────────────────────────────────────────────────────
static lv_obj_t             *s_scr        = NULL;
static lv_obj_t             *s_text_lbl   = NULL;
static lv_obj_t             *s_meter      = NULL;
static lv_meter_indicator_t *s_arc_ind    = NULL;
static lv_timer_t           *s_timer      = NULL;

// ── State ─────────────────────────────────────────────────────────
static uint32_t s_start_ms   = 0;
static bool     s_text_shown = false;
static bool     s_done       = false;

// ─────────────────────────────────────────────────────────────────
// BMP loader — exact copy from gauge_screen_init()
// ─────────────────────────────────────────────────────────────────

static void load_bmp(void) {
    FILE *f = fopen(BMP_PATH, "rb");
    if (!f) { ESP_LOGW(TAG, "BMP not found: %s", BMP_PATH); return; }

    uint8_t hdr[54];
    if (fread(hdr, 1, 54, f) != 54 || hdr[0] != 'B' || hdr[1] != 'M') {
        ESP_LOGE(TAG, "Invalid BMP header"); fclose(f); return;
    }

    uint32_t offset = hdr[10]|(hdr[11]<<8)|(hdr[12]<<16)|(hdr[13]<<24);
    int32_t  w   = (int32_t)(hdr[18]|(hdr[19]<<8)|(hdr[20]<<16)|(hdr[21]<<24));
    int32_t  h   = (int32_t)(hdr[22]|(hdr[23]<<8)|(hdr[24]<<16)|(hdr[25]<<24));
    uint16_t bpp = hdr[28]|(hdr[29]<<8);

    if (w != BMP_W || abs(h) != BMP_H || (bpp != 24 && bpp != 32)) {
        ESP_LOGE(TAG, "BMP wrong format"); fclose(f); return;
    }

    if (!s_bg_buf)
        s_bg_buf = (uint16_t*)heap_caps_malloc(BMP_W*BMP_H*2, MALLOC_CAP_SPIRAM);
    if (!s_bg_buf) { ESP_LOGE(TAG, "PSRAM alloc failed"); fclose(f); return; }

    fseek(f, offset, SEEK_SET);
    int Bpp = bpp/8, stride = ((w*Bpp+3)/4)*4;
    uint8_t *row = (uint8_t*)malloc(stride);
    if (!row) { fclose(f); return; }

    for (int r = 0; r < BMP_H; r++) {
        fread(row, 1, stride, f);
        int dst_row = (h > 0) ? (BMP_H-1-r) : r;
        uint16_t *dst = s_bg_buf + dst_row*BMP_W;
        for (int c = 0; c < BMP_W; c++) {
            uint8_t b=row[c*Bpp], g=row[c*Bpp+1], rv=row[c*Bpp+2];
            dst[c] = ((uint16_t)(rv&0xF8)<<8)|((uint16_t)(g&0xFC)<<3)|(b>>3);
        }
    }
    free(row); fclose(f);

    s_bg_dsc.header.cf          = LV_IMG_CF_TRUE_COLOR;
    s_bg_dsc.header.always_zero = 0;
    s_bg_dsc.header.reserved    = 0;
    s_bg_dsc.header.w           = BMP_W;
    s_bg_dsc.header.h           = BMP_H;
    s_bg_dsc.data_size          = BMP_W*BMP_H*2;
    s_bg_dsc.data               = (const uint8_t*)s_bg_buf;
    s_bg_loaded = true;
    ESP_LOGI(TAG, "BMP loaded OK");
}

// ─────────────────────────────────────────────────────────────────
// Transition
// ─────────────────────────────────────────────────────────────────

static void go_to_scan(lv_timer_t *t) {
    lv_timer_del(t);
    scan_screen_create();
}

static void cb_skip(lv_event_t *e) {
    (void)e;
    if (s_done) return;
    s_done = true;
    if (s_timer) { lv_timer_del(s_timer); s_timer = NULL; }
    Set_Backlight(100);
    lv_timer_create(go_to_scan, 20, NULL);
}

// ─────────────────────────────────────────────────────────────────
// Arc sweep
// ─────────────────────────────────────────────────────────────────

static void update_arc(uint32_t elapsed) {
    if (!s_meter || !s_arc_ind) return;

    ble_app_state_t st = ble_get_state();

    if (st == BLE_STATE_POLLING) {
        lv_meter_set_indicator_start_value(s_meter, s_arc_ind, 0);
        lv_meter_set_indicator_end_value(s_meter,   s_arc_ind, 6500);
        return;
    }

    uint32_t cycle = elapsed % 2400;
    bool     rev   = cycle >= 1200;
    uint32_t phase = rev ? (2400-cycle) : cycle;
    int pos   = (int)(phase * 6500 / 1200);
    int start = pos-750 < 0    ? 0    : pos-750;
    int end   = pos+750 > 6500 ? 6500 : pos+750;

    lv_meter_set_indicator_start_value(s_meter, s_arc_ind, start);
    lv_meter_set_indicator_end_value(s_meter,   s_arc_ind, end);
}

// ─────────────────────────────────────────────────────────────────
// Tick — 50ms
// ─────────────────────────────────────────────────────────────────

static void boot_tick(lv_timer_t *t) {
    (void)t;
    if (s_done) return;

    uint32_t elapsed = lv_tick_get() - s_start_ms;

    // Backlight fade in — ramp from 0 to 100 over BOOT_FADEIN_MS
    if (elapsed <= BOOT_FADEIN_MS) {
        uint8_t bl = (uint8_t)(elapsed * 100 / BOOT_FADEIN_MS);
        Set_Backlight(bl);
    }

    // Arc sweep
    update_arc(elapsed);

    // Text fade in
    if (elapsed >= BOOT_TEXT_APPEAR_MS) {
        if (!s_text_shown) {
            s_text_shown = true;
            lv_obj_clear_flag(s_text_lbl, LV_OBJ_FLAG_HIDDEN);
        }
        uint32_t te = elapsed - BOOT_TEXT_APPEAR_MS;
        int opa = (int)(te * 255 / BOOT_TEXT_FADE_MS);
        if (opa > 255) opa = 255;
        lv_obj_set_style_opa(s_text_lbl, (lv_opa_t)opa, LV_PART_MAIN);
    }

    // Transition
    if (elapsed >= BOOT_DURATION_MS) {
        s_done = true;
        lv_timer_del(s_timer);
        s_timer = NULL;
        Set_Backlight(100);
        lv_timer_create(go_to_scan, 20, NULL);
    }
}

// ─────────────────────────────────────────────────────────────────
// Public
// ─────────────────────────────────────────────────────────────────

void boot_screen_create(void) {
    s_text_shown = false;
    s_done       = false;

    // Start with backlight off
    Set_Backlight(0);

    load_bmp();

    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_scr, cb_skip, LV_EVENT_CLICKED, NULL);

    // BMP background
    if (s_bg_loaded) {
        lv_obj_t *bg = lv_img_create(s_scr);
        lv_img_set_src(bg, &s_bg_dsc);
        lv_obj_set_pos(bg, 0, 0);
    }

    // Arc meter — transparent over BMP
    s_meter = lv_meter_create(s_scr);
    lv_obj_set_size(s_meter, 420, 420);
    lv_obj_align(s_meter, LV_ALIGN_CENTER, 0, 5);
    lv_obj_set_style_bg_opa(s_meter,     LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_opa(s_meter, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_meter, 10, LV_PART_MAIN);
    lv_obj_clear_flag(s_meter, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_meter, LV_OBJ_FLAG_CLICKABLE);

    lv_meter_scale_t *scale = lv_meter_add_scale(s_meter);
    lv_meter_set_scale_range(s_meter, scale, 0, 6500, 280, 130);
    lv_meter_set_scale_ticks(s_meter, scale, 0, 0, 0, lv_color_black());

    s_arc_ind = lv_meter_add_arc(s_meter, scale, 6,
                                  lv_color_hex(0x60C1E4), 0);
    lv_meter_set_indicator_start_value(s_meter, s_arc_ind, 0);
    lv_meter_set_indicator_end_value(s_meter,   s_arc_ind, 1500);

    // "OBD2\nCar Gauge"
    s_text_lbl = lv_label_create(s_scr);
    lv_label_set_text(s_text_lbl, "OBD2\nCar Gauge");
    lv_obj_set_style_text_font(s_text_lbl, &lv_font_montserrat_40, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_text_lbl, lv_color_hex(0x60C1E4), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_text_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(s_text_lbl, 10, LV_PART_MAIN);
    lv_obj_set_style_opa(s_text_lbl, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_width(s_text_lbl, 320);
    lv_obj_align(s_text_lbl, LV_ALIGN_CENTER, 0, 60);
    lv_obj_add_flag(s_text_lbl, LV_OBJ_FLAG_HIDDEN);

    lv_scr_load(s_scr);
    s_start_ms = lv_tick_get();
    s_timer    = lv_timer_create(boot_tick, 50, NULL);

    ESP_LOGI(TAG, "Boot screen started");
}