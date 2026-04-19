#pragma once

// ── Scan screen — 480x480 round display ─────────────────────────
// Button-driven UI: UP / DOWN to scroll, long press to connect.
// Call scan_screen_create() once after LVGL init.
// The screen auto-transitions to gauge screen on successful connect.

void scan_screen_create(void);
void scan_screen_update(void);   // call periodically from LVGL loop (optional)
