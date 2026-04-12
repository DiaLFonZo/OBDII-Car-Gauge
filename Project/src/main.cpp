#include "Arduino.h"
#include "Display_ST7701.h"
#include "Touch_GT911.h"
#include "ble.h"
#include <lvgl.h>

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
static int lastCount = -1;
static int listOffset = 0;
static AppState lastState = STATE_BOOT;
#define VISIBLE_ITEMS 4

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

  // Title
  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "Select OBD2 Adapter");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 60);

  // Up button
  lv_obj_t *btn_up = lv_btn_create(scr);
  lv_obj_set_size(btn_up, 80, 50);
  lv_obj_align(btn_up, LV_ALIGN_TOP_MID, 0, 110);
  lv_obj_t *lbl_up = lv_label_create(btn_up);
  lv_label_set_text(lbl_up, LV_SYMBOL_UP);
  lv_obj_center(lbl_up);
  lv_obj_add_event_cb(btn_up, [](lv_event_t *e) {
    if (listOffset > 0) { listOffset--; rebuildScanList(getDeviceCount()); }
  }, LV_EVENT_CLICKED, nullptr);

  // Device buttons
  for (int i = 0; i < VISIBLE_ITEMS; i++) {
    int idx = listOffset + i;
    if (idx >= count) break;
    BLEDeviceEntry *dev = getDevice(idx);
    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 340, 55);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 170 + i * 62);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, dev->name.c_str());
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, device_btn_cb, LV_EVENT_CLICKED, (void*)(intptr_t)idx);
  }

  // Down button
  lv_obj_t *btn_dn = lv_btn_create(scr);
  lv_obj_set_size(btn_dn, 80, 50);
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
  lv_obj_t *label = lv_label_create(scr);
  lv_label_set_text(label, msg);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
  lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  LCD_Init();
  Touch_Init();

  // LVGL init
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

  // Touch input
  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type    = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = lvgl_touch_read;
  lv_indev_drv_register(&indev_drv);

  // LVGL timer
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

  // Handle connecting state
  static bool connectStarted = false;
  if (appState == STATE_CONNECTING && !connectStarted) {
    connectStarted = true;
    showStatus("Connecting...");
    lv_timer_handler();
    connectToSelectedDevice(appState);
    connectStarted = false;
  }

  // Update scan list when new devices found
  int count = getDeviceCount();
  if (count != lastCount) {
    lastCount = count;
    listOffset = 0;
    rebuildScanList(count);
  }

  // Show status when state changes
  if (appState != lastState) {
    lastState = appState;
    if (appState == STATE_CONNECTING)
      showStatus("Connecting...");
    else if (appState == STATE_INIT_ELM)
      showStatus("Initializing...");
    else if (appState == STATE_GAUGE)
      showStatus("Connected!");
    else if (appState == STATE_SCANNING) {
      lastCount = -1;
      listOffset = 0;
    }
  }
}