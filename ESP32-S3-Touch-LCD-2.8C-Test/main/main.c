#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "TCA9554PWR.h"
#include "PCF85063.h"
#include "QMI8658.h"
#include "ST7701S.h"
#include "GT911.h"
#include "SD_MMC.h"
#include "LVGL_Driver.h"
//#include "LVGL_Example.h"
#include "BAT_Driver.h"
#include "ble.h"
#include "scan_screen.h"

// ── Core 0: sensors + BLE ────────────────────────────────────────
void Driver_Loop(void *parameter) {
    while (1) {
        QMI8658_Loop();
        RTC_Loop();
        BAT_Get_Volts();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelete(NULL);
}

void Driver_Init(void) {
    Flash_Searching();
    BAT_Init();
    I2C_Init();
    PCF85063_Init();
    QMI8658_Init();
    EXIO_Init();
    xTaskCreatePinnedToCore(Driver_Loop, "drivers", 4096, NULL, 3, NULL, 0);
}

// ── Core 1: LVGL display ─────────────────────────────────────────
SemaphoreHandle_t lvgl_mutex;

void LVGL_Loop(void *parameter) {
    while (1) {
        if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            lv_timer_handler();
            xSemaphoreGive(lvgl_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelete(NULL);
}

// ── app_main ─────────────────────────────────────────────────────
void app_main(void) {
    // NVS must be initialized before BLE
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    lvgl_mutex = xSemaphoreCreateMutex();

    Driver_Init();
    LCD_Init();
    Touch_Init();
    SD_Init();
    LVGL_Init();

    // Show scan screen first
    scan_screen_create();

    // Start BLE on Core 0 (scans, connects, polls)
    ble_obd_start();

    // LVGL handler on Core 1
    xTaskCreatePinnedToCore(LVGL_Loop, "lvgl", 4096, NULL, 2, NULL, 1);
}