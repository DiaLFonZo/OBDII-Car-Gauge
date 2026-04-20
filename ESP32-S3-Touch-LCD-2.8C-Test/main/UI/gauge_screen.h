#pragma once

// ── Gauge screen — replaces debug_screen ─────────────────────────
// Call gauge_screen_init() once after SD and LVGL are ready.
// Call gauge_screen_show() to display (after BLE connects).
// Long press anywhere on gauge → next page.

void gauge_screen_init(void);   // loads BMP from SD, call after SD_Init()
void gauge_screen_show(void);   // loads first page, starts update timer
