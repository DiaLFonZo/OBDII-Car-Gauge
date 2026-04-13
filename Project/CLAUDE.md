# OBDII Car Gauge — Claude Context

## Project Goals
- Read OBDII data (RPM, speed, coolant temp, etc.) via BLE
- Display real-time gauges on a round 480x480 touch screen
- Hardware: Waveshare ESP32-S3-Touch-LCD-2.8C

## Rules for Claude
- This is an ongoing project — never rewrite entire files unless explicitly asked
- Suggest incremental changes only
- Always specify which file and function a change belongs in
- Show diffs or sections to replace, not full file rewrites
- Keep naming consistent with existing code
- Ask before guessing if context is missing

## Toolchain
- PlatformIO + VS Code
- Platform: pioarduino espressif32 53.3.10
- Framework: Arduino-ESP32 3.1.0
- LVGL 8.x
- NimBLE-Arduino 1.4.2

## Key Files
- `src/main.cpp`             — UI + app loop + LVGL
- `src/obd.cpp / obd.h`      — ELM327 + PID polling
- `src/ble.cpp / ble.h`      — BLE scan + connect
- `src/pids.h`               — PID definitions
- `src/Display_ST7701.cpp/h` — ST7701S display driver
- `src/TCA9554PWR.cpp/h`     — I2C GPIO expander
- `src/Touch_GT911.cpp/h`    — GT911 touch driver
- `src/lv_conf.h`            — LVGL config
- `platformio.ini`           — Build config

## Working Features
- Display init (ST7701 + TCA9554 expander)
- LVGL 8.x rendering + touch (GT911)
- BLE scan + device list UI with up/down navigation
- BLE connect to Veepeak OBDCheck BLE
- ELM327 init + prompt-based PID polling
- RPM gauge displaying live values

---

## Hardware — Waveshare ESP32-S3-Touch-LCD-2.8C

**MCU:** ESP32-S3 (dual-core LX7, 240MHz), 16MB Flash, 8MB OPI PSRAM

**Display:** 2.8" round IPS 480x480, ST7701S driver, 16-bit RGB parallel  
Backlight: GPIO6 (PWM/LEDC) | SPI init: CLK=GPIO2, MOSI=GPIO1

**RGB Data Pins:**  
- Red:   GPIO46, 3, 8, 18, 17  
- Green: GPIO14, 13, 12, 11, 10, 9  
- Blue:  GPIO5, 45, 48, 47, 21  
- DE: GPIO40 | HSYNC: GPIO38 | VSYNC: GPIO39 | PCLK: GPIO41

**Via TCA9554 I2C GPIO Expander (addr 0x20):**  
- LCD Reset: EXIO_PIN1 | Touch Reset: EXIO_PIN2  
- LCD CS: EXIO_PIN3 | Buzzer: EXIO_PIN8 (LOW = silent)

**I2C Bus:** SDA=GPIO15, SCL=GPIO7  
**Touch:** GT911, interrupt GPIO16

### ⚠️ Critical Boot Sequence
`LCD_Init()` MUST call `I2C_Init()` → `delay(120)` → `TCA9554PWR_Init(0x00)` → `ST7701_Reset()`  
Skipping this order = black screen on cold boot.

---

## OBD Adapter — Veepeak OBDCheck BLE

- Protocol: ELM327 v1.4
- Bluetooth: BLE 4.0 (NOT Classic)
- BLE name: `VEEPEAK`
- Service UUID: `FFF0`
- TX characteristic: `FFF2`
- RX characteristic: `FFF1`

**ELM327 Init Sequence:**  
`ATZ` → `ATE0` → `ATS0` → `ATL0` → `ATH0` → `ATAL` → `ATSP0`

**Polling Method:** Prompt-based — send PID, wait for `>` in notify callback, parse response, advance to next PID.

---

## PlatformIO Config Summary
```ini