#include "Arduino.h"
#include "Display_ST7701.h"
#include "Touch_GT911.h"
#include "ble.h"
#include "obd.h"
#include <lvgl.h>

extern int gaugePage;

AppState appState = STATE_BOOT;

// ── LVGL flush callback ──────────────────────────────────────────
static void lvgl_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
  LCD_addWindow(area->x1, area->y1, area->x2, area->y2, (uint8_t*)color_p);
  lv_disp_flush_ready(drv);
}

// ── LVGL tick ────────────────────────────────────────────────────
static void lvgl_tick(void *arg) { lv_tick_inc(5); }

// ── LVGL touch input ─────────────────────────────────────────────
static void lvgl_touch_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  uint16_t x, y, strength;
  uint8_t touchCount;
  Touch_Read_Data();
  Touch_Get_XY(&x, &y, &strength, &touchCount, 1);
  if (touchCount > 0) {
    data->point.x = x;
    data->point.y = y;
    data->state   = LV_INDEV_STATE_PR;
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

// ── Globals ──────────────────────────────────────────────────────
static int lastCount  = -1;
static int listOffset = 0;
static AppState lastState = STATE_BOOT;
#define VISIBLE_ITEMS 4

// ── Gauge screen objects ──────────────────────────────────────────
static lv_obj_t *gaugeArc   = nullptr;
static lv_obj_t *gaugeValue = nullptr;
static lv_obj_t *gaugeUnit  = nullptr;
static lv_obj_t *gaugeName  = nullptr;
static int        lastGaugePage = -1;

// ── Device button callback ────────────────────────────────────────
static void device_btn_cb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  setSelectedIndex(idx);
  appState = STATE_CONNECTING;
}

// ── Rebuild scan list UI ──────────────────────────────────────────
static void rebuildScanList(int count) {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_clean(scr);

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "Select OBD2 Adapter");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 60);

  lv_obj_t *btn_up = lv_btn_create(scr);
  lv_obj_set_size(btn_up, 80, 40);
  lv_obj_align(btn_up, LV_ALIGN_TOP_MID, 0, 110);
  lv_obj_t *lbl_up = lv_label_create(btn_up);
  lv_label_set_text(lbl_up, LV_SYMBOL_UP);
  lv_obj_center(lbl_up);
  lv_obj_add_event_cb(btn_up, [](lv_event_t *e) {
    if (listOffset > 0) { listOffset--; rebuildScanList(getDeviceCount()); }
  }, LV_EVENT_CLICKED, nullptr);

  for (int i = 0; i < VISIBLE_ITEMS; i++) {
    int idx = listOffset + i;
    if (idx >= count) break;
    BLEDeviceEntry *dev = getDevice(idx);
    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 340, 40);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 170 + i * 62);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, dev->name.c_str());
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, device_btn_cb, LV_EVENT_CLICKED, (void*)(intptr_t)idx);
  }

  lv_obj_t *btn_dn = lv_btn_create(scr);
  lv_obj_set_size(btn_dn, 80, 40);
  lv_obj_align(btn_dn, LV_ALIGN_BOTTOM_MID, 0, -20);
  lv_obj_t *lbl_dn = lv_label_create(btn_dn);
  lv_label_set_text(lbl_dn, LV_SYMBOL_DOWN);
  lv_obj_center(lbl_dn);
  lv_obj_add_event_cb(btn_dn, [](lv_event_t *e) {
    if (listOffset + VISIBLE_ITEMS < getDeviceCount()) { listOffset++; rebuildScanList(getDeviceCount()); }
  }, LV_EVENT_CLICKED, nullptr);
}

// ── Show status screen ────────────────────────────────────────────
static void showStatus(const char* msg) {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_clean(scr);
  gaugeArc = gaugeValue = gaugeUnit = gaugeName = nullptr;
  lv_obj_t *label = lv_label_create(scr);
  lv_label_set_text(label, msg);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
  lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
}

// ── Build gauge screen ────────────────────────────────────────────
static void buildGaugeScreen(int pidIndex) {
  const PIDDef &pid = PIDS[pidIndex];
  lv_obj_t *scr = lv_scr_act();
  lv_obj_clean(scr);

  // Black background
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

  // Arc — red, sweeps 240 degrees
  gaugeArc = lv_arc_create(scr);
  lv_obj_set_size(gaugeArc, 420, 420);
  lv_obj_center(gaugeArc);
  lv_arc_set_rotation(gaugeArc, 150);
  lv_arc_set_bg_angles(gaugeArc, 0, 240);
  lv_arc_set_value(gaugeArc, 0);
  lv_arc_set_range(gaugeArc, 0, 100);
  lv_obj_set_style_arc_color(gaugeArc, lv_color_make(60, 0, 0), LV_PART_MAIN);
  lv_obj_set_style_arc_color(gaugeArc, lv_color_make(255, 0, 0), LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(gaugeArc, 25, LV_PART_MAIN);
  lv_obj_set_style_arc_width(gaugeArc, 25, LV_PART_INDICATOR);
  lv_obj_remove_style(gaugeArc, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(gaugeArc, LV_OBJ_FLAG_CLICKABLE);

  // Large value label
  gaugeValue = lv_label_create(scr);
  lv_label_set_text(gaugeValue, "---");
  lv_obj_set_style_text_font(gaugeValue, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(gaugeValue, lv_color_make(255, 0, 0), 0);
  lv_obj_align(gaugeValue, LV_ALIGN_CENTER, 0, -20);

  // Unit label
  gaugeUnit = lv_label_create(scr);
  lv_label_set_text(gaugeUnit, pid.unit);
  lv_obj_set_style_text_font(gaugeUnit, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(gaugeUnit, lv_color_make(255, 0, 0), 0);
  lv_obj_align(gaugeUnit, LV_ALIGN_CENTER, 0, 40);

  // PID name
  gaugeName = lv_label_create(scr);
  lv_label_set_text(gaugeName, pid.name);
  lv_obj_set_style_text_font(gaugeName, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(gaugeName, lv_color_white(), 0);
  lv_obj_align(gaugeName, LV_ALIGN_CENTER, 0, 90);

  lastGaugePage = pidIndex;
}

// ── Update gauge values ───────────────────────────────────────────
static void updateGauge(int pidIndex) {
  if (!gaugeArc || !gaugeValue) return;
  if (pidIndex != lastGaugePage) {
    buildGaugeScreen(pidIndex);
    return;
  }

  const PIDDef &pid = PIDS[pidIndex];
  OBDValues &v = getOBDValues();

  if (!v.hasData[pidIndex]) return;

  float val = v.values[pidIndex];
  char buf[32];
  snprintf(buf, sizeof(buf), "%.0f", val);
  lv_label_set_text(gaugeValue, buf);

  // Map value to arc 0-100
  float pct = (val - pid.valMin) / (pid.valMax - pid.valMin) * 100.0f;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  lv_arc_set_value(gaugeArc, (int)pct);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  LCD_Init();
  Touch_Init();

  lv_init();
  static lv_color_t buf[480 * 10];
  static lv_disp_draw_buf_t draw_buf;
  lv_disp_draw_buf_init(&draw_buf, buf, NULL, 480 * 10);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res  = 480;
  disp_drv.ver_res  = 480;
  disp_drv.flush_cb = lvgl_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type    = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = lvgl_touch_read;
  lv_indev_drv_register(&indev_drv);

  const esp_timer_create_args_t timer_args = { .callback = lvgl_tick, .name = "lvgl" };
  esp_timer_handle_t timer;
  esp_timer_create(&timer_args, &timer);
  esp_timer_start_periodic(timer, 5000);

  showStatus("Scanning...");
  initBLE();
  startScan();
  appState = STATE_SCANNING;
}

void loop() {
  lv_timer_handler();
  delay(5);

  // Handle connecting
  static bool connectStarted = false;
  if (appState == STATE_CONNECTING && !connectStarted) {
    connectStarted = true;
    showStatus("Connecting...");
    lv_timer_handler();
    connectToSelectedDevice(appState);
    connectStarted = false;
  }

  // Handle OBD
  handleOBD(appState);

  // Update scan list
  if (appState == STATE_SCANNING) {
    int count = getDeviceCount();
    if (count != lastCount) {
      lastCount = count;
      listOffset = 0;
      rebuildScanList(count);
    }
  }

  // Show status on state change
  if (appState != lastState) {
    lastState = appState;
    if (appState == STATE_CONNECTING)
      showStatus("Connecting...");
    else if (appState == STATE_INIT_ELM)
      showStatus("Initializing...");
    else if (appState == STATE_GAUGE) {
      buildGaugeScreen(0);  // start with first PID
    } else if (appState == STATE_SCANNING) {
      lastCount = -1;
      listOffset = 0;
    }
  }

  // Update gauge
  if (appState == STATE_GAUGE) {
    updateGauge(gaugePage);
  }
}