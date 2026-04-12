#pragma once

// ═══════════════════════════════════════════════════════════════
//  hal.h — Hardware Abstraction Layer target selection
//
//  Uncomment ONE target before compiling.
//  This controls which input_*.cpp and ui_*.cpp are active.
// ═══════════════════════════════════════════════════════════════

//#define TARGET_CYBERPI      // CyberPi: ST7735 128x128, AW9523B, joystick + 2 buttons
//#define TARGET_HELTEC     // Heltec WiFi LoRa 32 V3: SSD1306 128x64, 1 button
#define TARGET_WAVESHARE  // Waveshare ESP32-S3-Touch-LCD-2.8C: 480x480 round, touch

// ── Validate ────────────────────────────────────────────────────
#if !defined(TARGET_CYBERPI) && !defined(TARGET_HELTEC) && !defined(TARGET_WAVESHARE)
  #error "No hardware target defined in hal.h"
#endif
