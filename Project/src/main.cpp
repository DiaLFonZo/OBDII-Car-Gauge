#include "Arduino.h"
#include "Display_ST7701.h"
#include "Touch_GT911.h"
#include "SD_Card.h"
#include "ble.h"
#include "obd.h"
#include <lvgl.h>
#include <Preferences.h>
#include <atomic>

LV_FONT_DECLARE(font_dseg7_48);
LV_FONT_DECLARE(font_dseg7_55);
LV_FONT_DECLARE(font_dseg7_65);
LV_FONT_DECLARE(font_dseg7_75);

// ── Shared state (atomic = hardware memory barrier on Xtensa dual-core) ──
std::atomic<int> appState{STATE_BOOT};
extern int gaugePage;


// ── LVGL mutex (guards lv_timer_handler vs task LVGL calls) ──
static SemaphoreHandle_t lvgl_mutex = NULL;
#define LVGL_LOCK()   xSemaphoreTake(lvgl_mutex, portMAX_DELAY)
#define LVGL_UNLOCK() xSemaphoreGive(lvgl_mutex)

// ── LVGL buffers ──────────────────────────────────────────────
static lv_color_t *lvgl_buf1 = nullptr;
static lv_color_t *lvgl_buf2 = nullptr;

// ── LVGL flush ────────────────────────────────────────────────
static void lvgl_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
  LCD_addWindow(area->x1, area->y1, area->x2, area->y2, (uint8_t*)color_p);
  lv_disp_flush_ready(drv);
}

// ── LVGL tick ─────────────────────────────────────────────────
static void lvgl_tick(void*) { lv_tick_inc(5); }

// ── LVGL touch ────────────────────────────────────────────────
static void lvgl_touch_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  uint16_t x, y, strength;
  uint8_t  cnt;
  Touch_Read_Data();
  Touch_Get_XY(&x, &y, &strength, &cnt, 1);
  if (cnt > 0) {
    data->point.x = x;
    data->point.y = y;
    data->state   = LV_INDEV_STATE_PR;
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

// ── Background image (loaded from SD) ─────────────────────────
static uint16_t*    bgBuf    = nullptr;
static lv_img_dsc_t bgDsc    = {};
static bool         bgLoaded = false;

// ── UI state ──────────────────────────────────────────────────
static volatile AppState lastUIState = STATE_BOOT;
static lv_obj_t *rawLabels[MAX_PIDS]     = {nullptr};
static lv_obj_t *rawScreen               = nullptr;
static lv_obj_t *rpmScreen               = nullptr;
static lv_obj_t *rpmLabel                = nullptr;
static lv_obj_t *rpmMeter                = nullptr;
static lv_meter_indicator_t *rpmNeedle   = nullptr;

// ── On-screen BLE status (written by task, read by UI) ────────
char             bleStatusLine[64] = "";
static lv_obj_t *statusSubLbl      = nullptr;

// ── Reset all screen-object pointers (call before lv_obj_clean) ──
static void resetScreenPtrs() {
  rawScreen    = nullptr;
  rpmScreen    = nullptr;
  rpmMeter     = nullptr;
  rpmLabel     = nullptr;
  rpmNeedle    = nullptr;
  statusSubLbl = nullptr;
  memset(rawLabels, 0, sizeof(rawLabels));
}

// ── Device button callback ────────────────────────────────────
static void device_btn_cb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  setSelectedIndex(idx);
  appState.store((int)STATE_CONNECTING);
  Serial.printf("[UI] device_btn_cb fired, idx=%d, appState set to %d\n", idx, (int)STATE_CONNECTING);
}

// ── showStatus ─── title + live subtitle updated by task ──────
static void showStatus(const char* title) {
  LVGL_LOCK();
  lv_obj_t *scr = lv_scr_act();
  resetScreenPtrs();
  lv_obj_clean(scr);

  lv_obj_t *lbl = lv_label_create(scr);
  lv_label_set_text(lbl, title);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
  lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -40);

  statusSubLbl = lv_label_create(scr);
  lv_label_set_text(statusSubLbl, bleStatusLine);
  lv_obj_set_style_text_font(statusSubLbl, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(statusSubLbl, lv_color_make(100, 180, 255), 0);
  lv_obj_set_style_text_align(statusSubLbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(statusSubLbl, 360);
  lv_obj_align(statusSubLbl, LV_ALIGN_CENTER, 0, 10);

  // Cancel button — escape if BLE stack hangs
  lv_obj_t *btn = lv_btn_create(scr);
  lv_obj_set_size(btn, 120, 40);
  lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -50);
  lv_obj_set_style_bg_color(btn, lv_color_make(80, 80, 80), 0);
  lv_obj_t *blbl = lv_label_create(btn);
  lv_label_set_text(blbl, "Cancel");
  lv_obj_set_style_text_font(blbl, &lv_font_montserrat_14, 0);
  lv_obj_center(blbl);
  lv_obj_add_event_cb(btn, [](lv_event_t*) {
    strncpy(bleStatusLine, "Cancelled", sizeof(bleStatusLine));
    appState.store((int)STATE_SCANNING);
  }, LV_EVENT_CLICKED, nullptr);

  LVGL_UNLOCK();
}

// ── showDeviceGrid ────────────────────────────────────────────
static void showDeviceGrid() {
  int count = getDeviceCount();
  LVGL_LOCK();
  lv_obj_t *scr = lv_scr_act();
  resetScreenPtrs();
  lv_obj_clean(scr);

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "Select OBD2 Adapter");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 55);

  if (count == 0) {
    lv_obj_t *none = lv_label_create(scr);
    lv_label_set_text(none, "No devices found.");
    lv_obj_set_style_text_font(none, &lv_font_montserrat_20, 0);
    lv_obj_align(none, LV_ALIGN_CENTER, 0, 0);
    LVGL_UNLOCK();
    return;
  }

  // Transparent scrollable container, 2-column flex wrap
  lv_obj_t *cont = lv_obj_create(scr);
  lv_obj_set_size(cont, 340, 320);
  lv_obj_align(cont, LV_ALIGN_CENTER, 0, 15);
  lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(cont, 0, 0);
  lv_obj_set_style_pad_all(cont, 0, 0);
  lv_obj_set_style_pad_gap(cont, 8, 0);
  lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_scroll_dir(cont, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_AUTO);

  int btnW = 160;
  int btnH = 58;

  for (int i = 0; i < count && i < MAX_DEVICES; i++) {
    BLEDeviceEntry *dev = getDevice(i);
    lv_obj_t *btn = lv_btn_create(cont);
    lv_obj_set_size(btn, btnW, btnH);

    lv_obj_t *lbl = lv_label_create(btn);
    String txt = (dev->name.length() > 0 && !dev->name.startsWith(dev->address.substring(0, 8)))
                 ? dev->name : dev->address.substring(0, 11);
    lv_label_set_text(lbl, txt.c_str());
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(lbl, btnW - 8);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, device_btn_cb, LV_EVENT_LONG_PRESSED, (void*)(intptr_t)i);
  }

  lv_obj_t *slbl = lv_label_create(scr);
  lv_label_set_text(slbl, LV_SYMBOL_LOOP " Scanning...");
  lv_obj_set_style_text_font(slbl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(slbl, lv_color_make(120, 120, 120), 0);
  lv_obj_align(slbl, LV_ALIGN_BOTTOM_MID, 0, -30);

  LVGL_UNLOCK();
}

// ── buildRawScreen ────────────────────────────────────────────
static void buildRawScreen() {
  LVGL_LOCK();
  lv_obj_t *scr = lv_scr_act();
  resetScreenPtrs();
  lv_obj_clean(scr);
  rawScreen = scr;

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "LIVE OBD2 VALUES");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(title, lv_color_make(180, 180, 180), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 50);

  // Swipe right → gauge page
  lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(scr, [](lv_event_t*) {
    if (lv_indev_get_gesture_dir(lv_indev_get_act()) == LV_DIR_RIGHT) gaugePage = 0;
  }, LV_EVENT_GESTURE, nullptr);

  int cols   = 2;
  int rowH   = 30;
  int colW   = 220;
  int startX = 20;
  int startY = 80;

  for (int i = 0; i < PID_COUNT; i++) {
    int col = i % cols;
    int row = i / cols;
    int x   = startX + col * colW;
    int y   = startY + row * rowH;
    if (y > 440) break;

    lv_obj_t *name = lv_label_create(scr);
    lv_label_set_text(name, PIDS[i].name);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(name, lv_color_make(150, 150, 150), 0);
    lv_obj_set_pos(name, x, y);

    rawLabels[i] = lv_label_create(scr);
    lv_label_set_text(rawLabels[i], "---");
    lv_obj_set_style_text_font(rawLabels[i], &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(rawLabels[i], lv_color_white(), 0);
    lv_obj_set_pos(rawLabels[i], x + 105, y);
  }

  lv_obj_t *disc = lv_btn_create(scr);
  lv_obj_set_size(disc, 130, 36);
  lv_obj_align(disc, LV_ALIGN_BOTTOM_MID, 68, -48);
  lv_obj_set_style_bg_color(disc, lv_color_make(160, 30, 30), 0);
  lv_obj_t *dlbl = lv_label_create(disc);
  lv_label_set_text(dlbl, LV_SYMBOL_CLOSE " Disconnect");
  lv_obj_set_style_text_font(dlbl, &lv_font_montserrat_14, 0);
  lv_obj_center(dlbl);
  lv_obj_add_event_cb(disc, [](lv_event_t*) {
    NimBLEClient *c = getBLEClient();
    if (c) c->disconnect();
    appState.store((int)STATE_SCANNING);
  }, LV_EVENT_CLICKED, nullptr);

  LVGL_UNLOCK();
}

// ── updateRawValues ───────────────────────────────────────────
static void updateRawValues() {
  if (!rawScreen) return;
  OBDValues &v = getOBDValues();
  LVGL_LOCK();
  for (int i = 0; i < PID_COUNT; i++) {
    if (!rawLabels[i] || !v.hasData[i]) continue;
    char buf[20];
    if (PIDS[i].isBoolean)
      snprintf(buf, sizeof(buf), "%s", v.values[i] > 0.5f ? "ON" : "OFF");
    else
      snprintf(buf, sizeof(buf), "%.1f %s", v.values[i], PIDS[i].unit);
    lv_label_set_text(rawLabels[i], buf);
    lv_obj_set_style_text_color(rawLabels[i],
      (PIDS[i].warn > 0 && v.values[i] >= PIDS[i].warn)
        ? lv_color_make(255, 60, 60)
        : lv_color_white(), 0);
  }
  LVGL_UNLOCK();
}

// ── loadBMPBackground — reads /gauge.bmp (24-bit) from SD into PSRAM ──
static bool loadBMPBackground() {
  if (SD_MMC.cardType() == CARD_NONE) {
    Serial.println("[BG] SD not mounted");
    return false;
  }

  File f = SD_MMC.open("/gauge_2.bmp");
  if (!f) { Serial.println("[BG] /gauge_2.bmp not found"); return false; }

  uint8_t hdr[54];
  if (f.read(hdr, 54) != 54 || hdr[0] != 'B' || hdr[1] != 'M') {
    Serial.println("[BG] not a valid BMP");
    f.close(); return false;
  }
  uint32_t offset = hdr[10] | (hdr[11]<<8) | (hdr[12]<<16) | (hdr[13]<<24);
  int32_t  w      = (int32_t)(hdr[18] | (hdr[19]<<8) | (hdr[20]<<16) | (hdr[21]<<24));
  int32_t  h      = (int32_t)(hdr[22] | (hdr[23]<<8) | (hdr[24]<<16) | (hdr[25]<<24));
  uint16_t bpp    = hdr[28] | (hdr[29]<<8);

  if (w != 480 || abs(h) != 480 || (bpp != 24 && bpp != 32)) {
    Serial.printf("[BG] bad BMP %dx%d %dbpp (need 480x480, 24 or 32-bit)\n", w, abs(h), bpp);
    f.close(); return false;
  }

  if (!bgBuf) bgBuf = (uint16_t*)heap_caps_malloc(480*480*2, MALLOC_CAP_SPIRAM);
  if (!bgBuf) { Serial.println("[BG] PSRAM alloc failed"); f.close(); return false; }

  f.seek(offset);
  int Bpp    = bpp / 8;                          // bytes per pixel: 3 or 4
  int stride = ((w * Bpp + 3) / 4) * 4;
  uint8_t *rowBuf = (uint8_t*)malloc(stride);
  if (!rowBuf) { f.close(); return false; }

  for (int r = 0; r < 480; r++) {
    f.read(rowBuf, stride);
    int dstRow = (h > 0) ? (479 - r) : r;   // BMP is bottom-up when h > 0
    uint16_t *dst = bgBuf + dstRow * 480;
    for (int c = 0; c < 480; c++) {
      uint8_t b = rowBuf[c*Bpp], g = rowBuf[c*Bpp+1], rv = rowBuf[c*Bpp+2];
      // byte 3 (alpha) is ignored for 32-bit
      dst[c] = ((uint16_t)(rv & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
    }
  }
  free(rowBuf);
  f.close();

  bgDsc.header.cf          = LV_IMG_CF_TRUE_COLOR;
  bgDsc.header.always_zero = 0;
  bgDsc.header.reserved    = 0;
  bgDsc.header.w           = 480;
  bgDsc.header.h           = 480;
  bgDsc.data_size          = 480 * 480 * 2;
  bgDsc.data               = (const uint8_t*)bgBuf;
  bgLoaded = true;
  Serial.println("[BG] loaded OK");
  return true;
}

// ── buildGaugeScreen — RPM gauge with background image ────────
static void buildGaugeScreen() {
  LVGL_LOCK();
  lv_obj_t *scr = lv_scr_act();
  resetScreenPtrs();
  lv_obj_clean(scr);
  rpmScreen = scr;

  // Background image
  if (bgLoaded) {
    lv_obj_t *bg = lv_img_create(scr);
    lv_img_set_src(bg, &bgDsc);
    lv_obj_set_pos(bg, 0, 0);
    lv_obj_add_flag(bg, LV_OBJ_FLAG_GESTURE_BUBBLE);
  } else {
    // Show why — visible even without a serial monitor
    lv_obj_t *dbg = lv_label_create(scr);
    lv_label_set_text(dbg, bgBuf ? "BG: fmt/dim err" :
                            (SD_MMC.cardType() == CARD_NONE) ? "BG: SD not mounted" :
                            "BG: file not found");
    lv_obj_set_style_text_font(dbg, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(dbg, lv_color_make(255, 100, 0), 0);
    lv_obj_align(dbg, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_add_flag(dbg, LV_OBJ_FLAG_GESTURE_BUBBLE);
  }

  // RPM meter — transparent over the background
  rpmMeter = lv_meter_create(scr);
  lv_obj_set_size(rpmMeter, 420, 420);
  lv_obj_align(rpmMeter, LV_ALIGN_CENTER, 0, 5);
  lv_obj_set_style_bg_opa(rpmMeter, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_opa(rpmMeter, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_pad_all(rpmMeter, 10, LV_PART_MAIN);
  lv_obj_add_flag(rpmMeter, LV_OBJ_FLAG_GESTURE_BUBBLE);

  lv_meter_scale_t *scale = lv_meter_add_scale(rpmMeter);
  lv_meter_set_scale_range(rpmMeter, scale, 0, 6500, 280, 130);
  lv_meter_set_scale_ticks(rpmMeter, scale, 66, 2, 10,
                           lv_color_make(60, 120, 180));
  lv_meter_set_scale_major_ticks(rpmMeter, scale, 5, 4, 20,
                                 lv_color_make(160, 210, 255), 8);
  lv_obj_set_style_text_opa(rpmMeter, LV_OPA_TRANSP, LV_PART_TICKS); // hide number labels

  // Normal arc hidden (width 0 = transparent)
  lv_meter_indicator_t *arcN = lv_meter_add_arc(rpmMeter, scale, 0,
                                                  lv_color_make(0, 0, 0), 0);
  lv_meter_set_indicator_start_value(rpmMeter, arcN, 0);
  lv_meter_set_indicator_end_value(rpmMeter, arcN, 5250);

  // Red arc: redline → max
  lv_meter_indicator_t *arcW = lv_meter_add_arc(rpmMeter, scale, 12,
                                                  lv_color_make(220, 30, 30), -8);
  lv_meter_set_indicator_start_value(rpmMeter, arcW, 5250);
  lv_meter_set_indicator_end_value(rpmMeter, arcW, 6500);

  // Needle
  rpmNeedle = lv_meter_add_needle_line(rpmMeter, scale, 3, lv_color_white(), -20);
  lv_meter_set_indicator_value(rpmMeter, rpmNeedle, 0);

  // Digital readout — fixed width + center align so digits never shift
  // Change GAUGE_DIGITS to set how many digits are shown (e.g. 4 → "0123")
  #define GAUGE_DIGITS 4
  rpmLabel = lv_label_create(scr);
  lv_label_set_text(rpmLabel, "0000");
  lv_obj_set_style_text_font(rpmLabel, &font_dseg7_65, 0);
  lv_obj_set_style_text_color(rpmLabel, lv_color_make(190, 249, 253), 0);
  lv_obj_set_style_text_align(rpmLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(rpmLabel, 220);          // wide enough for any 4-digit number
  // RPM Value
  lv_obj_align(rpmLabel, LV_ALIGN_CENTER, 0, 95);
  lv_obj_add_flag(rpmLabel, LV_OBJ_FLAG_GESTURE_BUBBLE);

  lv_obj_t *unit = lv_label_create(scr);
  lv_label_set_text(unit, "RPM");
  lv_obj_set_style_text_font(unit, &lv_font_montserrat_30, 0);
  lv_obj_set_style_text_color(unit, lv_color_make(96, 193, 228), 0);
  // RPM Label
  lv_obj_align(unit, LV_ALIGN_CENTER, 0, 180);
  lv_obj_add_flag(unit, LV_OBJ_FLAG_GESTURE_BUBBLE);

  // Swipe left → data page
  lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(scr, [](lv_event_t*) {
    if (lv_indev_get_gesture_dir(lv_indev_get_act()) == LV_DIR_LEFT) gaugePage = 1;
  }, LV_EVENT_GESTURE, nullptr);

  LVGL_UNLOCK();
}

// ── updateGaugeValues — update needle + digital label ─────────
static void updateGaugeValues() {
  if (!rpmScreen || !rpmMeter || !rpmLabel || !rpmNeedle) return;
  OBDValues &v = getOBDValues();
  if (!v.hasData[0]) return;           // index 0 = RPM in pids.h

  int rpm = (int)v.values[0];
  rpm = rpm < 0 ? 0 : (rpm > 6500 ? 6500 : rpm);

  LVGL_LOCK();
  lv_meter_set_indicator_value(rpmMeter, rpmNeedle, rpm);
  char buf[8];
  snprintf(buf, sizeof(buf), "%0*d", GAUGE_DIGITS, rpm);
  lv_label_set_text(rpmLabel, buf);
  LVGL_UNLOCK();
}

static void ble_obd_task(void*) {
#ifdef DEMO_MODE
  buildPIDList();
  OBDValues &dv = getOBDValues();
  for (int i = 0; i < PID_COUNT; i++) dv.hasData[i] = true;
  appState.store((int)STATE_GAUGE);
  for (uint32_t tick = 0;;tick++) {
    float t = tick * 0.005f;
    dv.values[0]  = 800.0f + 4700.0f * (0.5f + 0.5f * sinf(t));
    dv.values[1]  = dv.values[0] / 28.0f;
    dv.values[2]  = 82.0f + 3.0f * sinf(t * 0.1f);
    dv.values[3]  = 95.0f;  dv.values[4]  = 88.0f;
    dv.values[5]  = 22.0f;
    dv.values[6]  = (dv.values[0] - 800.0f) / 47.0f;
    dv.values[7]  = 101.0f;
    dv.values[8]  = 5.0f + 3.0f * sinf(t * 0.3f);
    dv.values[9]  = 101.3f; dv.values[10] = 14.2f;
    dv.values[11] = 0.0f;   dv.values[12] = 12.0f;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
#endif

  initBLE();
  startScan();

  static AppState taskLastState = STATE_BOOT;

  while (1) {
    AppState state = (AppState)appState.load();
    Serial.printf("[TASK] state=%d last=%d\n", (int)state, (int)taskLastState);

    // ── Scanning ──────────────────────────────────────────────
    if (state == STATE_SCANNING) {
      if (taskLastState != STATE_SCANNING)
        taskLastState = STATE_SCANNING;
    }

    // ── Connecting — attempt once per entry into this state ───
    else if (state == STATE_CONNECTING) {
      if (taskLastState != STATE_CONNECTING) {
        taskLastState = STATE_CONNECTING;
        vTaskDelay(pdMS_TO_TICKS(200));
        AppState s = (AppState)appState.load();
        connectToSelectedDevice(s);
        if (s != STATE_INIT_ELM)
          taskLastState = STATE_BOOT;
        appState.store((int)s);
      }
    }

    // ── OBD polling ───────────────────────────────────────────
    else if (state == STATE_INIT_ELM || state == STATE_GAUGE) {
      taskLastState = state;
      AppState s = (AppState)appState.load();
      handleOBD(s);
      appState.store((int)s);

      if (state == STATE_INIT_ELM && s == STATE_SCANNING)
        taskLastState = STATE_BOOT;
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ── setup ─────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  LCD_Init();
  Touch_Init();
  SD_Init();
  loadBMPBackground();   // load /gauge.bmp into PSRAM before LVGL starts

  lvgl_mutex = xSemaphoreCreateMutex();
  lv_init();

  lvgl_buf1 = (lv_color_t*)heap_caps_malloc(480 * 40 * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  lvgl_buf2 = (lv_color_t*)heap_caps_malloc(480 * 40 * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);

  static lv_disp_draw_buf_t draw_buf;
  lv_disp_draw_buf_init(&draw_buf, lvgl_buf1, lvgl_buf2, 480 * 40);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res  = 480;
  disp_drv.ver_res  = 480;
  disp_drv.flush_cb = lvgl_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  lv_theme_t *th = lv_theme_default_init(NULL,
    lv_palette_main(LV_PALETTE_RED),
    lv_palette_main(LV_PALETTE_GREY),
    true, &lv_font_montserrat_14);
  lv_disp_set_theme(NULL, th);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type             = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb          = lvgl_touch_read;
  indev_drv.long_press_time  = 700;
  lv_indev_drv_register(&indev_drv);

  const esp_timer_create_args_t ta = { .callback = lvgl_tick, .name = "lvgl" };
  esp_timer_handle_t timer;
  esp_timer_create(&ta, &timer);
  esp_timer_start_periodic(timer, 5000);

  appState.store((int)STATE_SCANNING);
  xTaskCreatePinnedToCore(ble_obd_task, "ble_obd", 32768, NULL, 3, NULL, 1);
}

// ── loop — UI only, core 0 ────────────────────────────────────
void loop() {
  LVGL_LOCK();
  lv_timer_handler();
  LVGL_UNLOCK();

  AppState state = (AppState)appState.load();

  // ── UI: react to state changes ────────────────────────────
  static int lastGaugePage = -1;
  if (state != lastUIState) {
    lastUIState   = state;
    lastGaugePage = -1;
    if (state == STATE_SCANNING) {
      showDeviceGrid();
    } else if (state == STATE_CONNECTING) {
      BLEDeviceEntry* dev = getDevice(getSelectedIndex());
      snprintf(bleStatusLine, sizeof(bleStatusLine), "%s",
               (dev && dev->name.length()) ? dev->name.c_str() : "...");
      showStatus("Connecting...");
    } else if (state == STATE_INIT_ELM) {
      strncpy(bleStatusLine, "Initializing ELM327...", sizeof(bleStatusLine));
      showStatus("Connected");
    }
  }

  // Refresh live subtitle (inside mutex — it's an LVGL call)
  if ((state == STATE_CONNECTING || state == STATE_INIT_ELM) && statusSubLbl) {
    LVGL_LOCK();
    lv_label_set_text(statusSubLbl, bleStatusLine);
    LVGL_UNLOCK();
  }

  // Hard watchdog: if the BLE task freezes inside connect(), force back to scan after 15 s
  static unsigned long connectWatchdog = 0;
  if (state == STATE_CONNECTING) {
    if (connectWatchdog == 0) connectWatchdog = millis();
    if (millis() - connectWatchdog > 15000) {
      snprintf(bleStatusLine, sizeof(bleStatusLine), "Timeout — forced back to scan");
      connectWatchdog = 0;
      appState.store((int)STATE_SCANNING);
    }
  } else {
    connectWatchdog = 0;
  }

  // Build gauge page on change
  if (state == STATE_GAUGE && gaugePage != lastGaugePage) {
    lastGaugePage = gaugePage;
    if (gaugePage == 0) buildGaugeScreen();
    else                buildRawScreen();
  }

  // Rebuild scan grid on first device or reset
  static int lastScanCount = -1;
  if (state == STATE_SCANNING) {
    int count = getDeviceCount();
    if (count != lastScanCount) {
      bool wasEmpty = (lastScanCount <= 0);
      lastScanCount = count;
      if (wasEmpty || count == 0) showDeviceGrid();
    }
  } else {
    lastScanCount = -1;
  }

  // Live gauge updates
  if (state == STATE_GAUGE) {
    if (gaugePage == 0) updateGaugeValues();
    else                updateRawValues();
  }

  vTaskDelay(pdMS_TO_TICKS(5));
}