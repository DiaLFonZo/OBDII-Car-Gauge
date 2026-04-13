#include "Arduino.h"
#include "Display_ST7701.h"
#include "Touch_GT911.h"
#include "ble.h"
#include "obd.h"
#include <lvgl.h>
#include <Preferences.h>

// ── Shared state ──────────────────────────────────────────────
volatile AppState appState = STATE_BOOT;
extern int gaugePage;

// ── LVGL mutex ────────────────────────────────────────────────
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

// ── UI state ──────────────────────────────────────────────────
static volatile AppState lastUIState = STATE_BOOT;
static lv_obj_t *rawLabels[MAX_PIDS] = {nullptr};
static lv_obj_t *rawScreen           = nullptr;

// ── Device button callback ────────────────────────────────────
static void device_btn_cb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  setSelectedIndex(idx);
  appState = STATE_CONNECTING;
}

// ── showStatus ────────────────────────────────────────────────
static void showStatus(const char* msg) {
  LVGL_LOCK();
  lv_obj_t *scr = lv_scr_act();
  lv_obj_clean(scr);
  rawScreen = nullptr;
  memset(rawLabels, 0, sizeof(rawLabels));
  lv_obj_t *lbl = lv_label_create(scr);
  lv_label_set_text(lbl, msg);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
  lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
  LVGL_UNLOCK();
}

// ── showDeviceGrid ────────────────────────────────────────────
static void showDeviceGrid() {
  int count = getDeviceCount();
  LVGL_LOCK();
  lv_obj_t *scr = lv_scr_act();
  lv_obj_clean(scr);
  rawScreen = nullptr;

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

  int cols   = 3;
  int btnW   = 120;
  int btnH   = 50;
  int padX   = 8;
  int padY   = 8;
  int startY = 110;
  int startX = (480 - (cols * btnW + (cols-1) * padX)) / 2;

  for (int i = 0; i < count && i < MAX_DEVICES; i++) {
    BLEDeviceEntry *dev = getDevice(i);
    int col = i % cols;
    int row = i / cols;
    int x   = startX + col * (btnW + padX);
    int y   = startY + row * (btnH + padY);

    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_set_size(btn, btnW, btnH);
    lv_obj_set_pos(btn, x, y);

    lv_obj_t *lbl = lv_label_create(btn);
    String txt = (dev->name.length() > 0 && !dev->name.startsWith(dev->address.substring(0,8)))
                 ? dev->name : dev->address.substring(0, 11);
    lv_label_set_text(lbl, txt.c_str());
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(lbl, btnW - 8);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, device_btn_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
  }

  // Rescan button
  lv_obj_t *rescan = lv_btn_create(scr);
  lv_obj_set_size(rescan, 160, 45);
  lv_obj_align(rescan, LV_ALIGN_BOTTOM_MID, 0, -30);
  lv_obj_t *rlbl = lv_label_create(rescan);
  lv_label_set_text(rlbl, LV_SYMBOL_REFRESH " Rescan");
  lv_obj_center(rlbl);
  lv_obj_add_event_cb(rescan, [](lv_event_t*) {
    appState = STATE_SCANNING;
  }, LV_EVENT_CLICKED, nullptr);

  LVGL_UNLOCK();
}

// ── buildRawScreen ────────────────────────────────────────────
static void buildRawScreen() {
  LVGL_LOCK();
  lv_obj_t *scr = lv_scr_act();
  lv_obj_clean(scr);
  rawScreen = scr;
  memset(rawLabels, 0, sizeof(rawLabels));

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "LIVE OBD2 VALUES");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(title, lv_color_make(180, 180, 180), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 50);

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
  lv_obj_set_size(disc, 130, 40);
  lv_obj_align(disc, LV_ALIGN_BOTTOM_MID, 0, -20);
  lv_obj_t *dlbl = lv_label_create(disc);
  lv_label_set_text(dlbl, LV_SYMBOL_CLOSE " Disconnect");
  lv_obj_set_style_text_font(dlbl, &lv_font_montserrat_14, 0);
  lv_obj_center(dlbl);
  lv_obj_add_event_cb(disc, [](lv_event_t*) {
    NimBLEClient *c = getBLEClient();
    if (c) c->disconnect();
    appState = STATE_SCANNING;
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

static void ble_obd_task(void*) {
  static bool     scanDone      = false;
  static uint32_t scanStartTime = 0;
  static AppState taskLastState = STATE_BOOT;

  while(1) {
    AppState state = appState;

    // ── Scanning ──────────────────────────────────────────────
    if (state == STATE_SCANNING) {
      if (taskLastState != STATE_SCANNING) {
        taskLastState = STATE_SCANNING;
        scanDone      = false;
        scanStartTime = millis();
        startScan();
      }
      if (!scanDone && millis() - scanStartTime >= 3000) {
        scanDone = true;
        stopScan();
        Preferences prefs;
        prefs.begin("obd", true);
        String savedMac = prefs.getString("mac", "");
        prefs.end();
        bool found = false;
        if (savedMac != "") {
          for (int i = 0; i < getDeviceCount(); i++) {
            if (getDevice(i)->address == savedMac) {
              setSelectedIndex(i);
              found = true;
              break;
            }
          }
        }
        if (found) {
          appState = STATE_CONNECTING;
        }
        // if not found, stay in STATE_SCANNING, UI will show grid
      }
    }

    // ── Connecting ────────────────────────────────────────────
    else if (state == STATE_CONNECTING) {
      if (taskLastState != STATE_CONNECTING) {
        taskLastState = STATE_CONNECTING;
        AppState s = appState;
        connectToSelectedDevice(s);
        appState = s;
      }
    }

    // ── OBD ───────────────────────────────────────────────────
    else if (state == STATE_INIT_ELM || state == STATE_GAUGE) {
      taskLastState = state;
      AppState s = appState;
      handleOBD(s);
      appState = s;
    }

    // ── Reset task state when returning to scanning ───────────
    if (state != STATE_SCANNING && appState == STATE_SCANNING) {
      taskLastState = STATE_BOOT;  // forces scan restart
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ── setup ─────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);
  LCD_Init();
  Touch_Init();

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
  indev_drv.type    = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = lvgl_touch_read;
  lv_indev_drv_register(&indev_drv);

  const esp_timer_create_args_t ta = { .callback = lvgl_tick, .name = "lvgl" };
  esp_timer_handle_t timer;
  esp_timer_create(&ta, &timer);
  esp_timer_start_periodic(timer, 5000);

  initBLE();
  showStatus("Scanning...");
  appState = STATE_SCANNING;

  // BLE/OBD on core 1
  xTaskCreatePinnedToCore(ble_obd_task, "ble_obd", 8192, NULL, 3, NULL, 1);
}

// ── loop — UI only, core 0 ────────────────────────────────────
void loop() {
  LVGL_LOCK();
  lv_timer_handler();
  LVGL_UNLOCK();

  AppState state = appState;

  // UI reacts to state changes
  if (state != lastUIState) {
    lastUIState = state;
    if (state == STATE_SCANNING)
      showStatus("Scanning...");
    else if (state == STATE_CONNECTING)
      showStatus("Connecting...");
    else if (state == STATE_INIT_ELM)
      showStatus("Initializing...");
    else if (state == STATE_GAUGE)
      buildRawScreen();
  }

  // Show device grid once scan is done and still in scanning state
  static bool gridShown = false;
  if (state == STATE_SCANNING) {
    // Check if scan has completed (deviceCount stable for 500ms)
    static int     lastCount    = -1;
    static uint32_t stableStart = 0;
    int count = getDeviceCount();
    if (count != lastCount) {
      lastCount    = count;
      stableStart  = millis();
      gridShown    = false;
    }
    if (!gridShown && count > 0 && millis() - stableStart > 500) {
      gridShown = true;
      showDeviceGrid();
    }
  } else {
    gridShown = false;
  }

  if (state == STATE_GAUGE)
    updateRawValues();

  vTaskDelay(pdMS_TO_TICKS(5));
}