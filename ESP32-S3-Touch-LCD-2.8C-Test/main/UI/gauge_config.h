#pragma once

#include <stdint.h>
#include <stdbool.h>

// ════════════════════════════════════════════════════════════════
//  gauge_config.h  — ALL visual parameters live here.
//  Edit this file to change colors, tick counts, warn zones, etc.
//  No need to touch gauge_screen.c
// ════════════════════════════════════════════════════════════════

// ── Page types ───────────────────────────────────────────────────
typedef enum {
    GAUGE_TYPE_ARC,      // needle meter + DSEG7 digital + BMP background
    GAUGE_TYPE_DUAL,     // two values side by side with bar + digital
    GAUGE_TYPE_DIGITAL,  // single large DSEG7 number
    GAUGE_TYPE_RAW,      // scrollable list of all active PID values
} gauge_type_t;

// ── Arc gauge config ─────────────────────────────────────────────
typedef struct {
    int      val_min;
    int      val_max;
    int      angle_range;       // total sweep degrees (280 = almost full circle)
    int      angle_start;       // start angle clockwise from bottom (130)

    // Minor ticks
    int      minor_count;       // total number of minor ticks (66)
    int      minor_width;       // line width px (2)
    int      minor_len;         // line length px (10)
    uint32_t minor_color;       // 0xRRGGBB

    // Major ticks (every Nth minor tick)
    int      major_nth;         // every Nth is major (5)
    int      major_width;       // line width px (4)
    int      major_len;         // line length px (20)
    uint32_t major_color;       // 0xRRGGBB
    bool     show_labels;       // show numeric labels on major ticks

    // Warning arc (red zone at high end)
    int      warn_start;        // value where warn arc starts (0 = disabled)
    int      warn_end;
    int      warn_arc_width;    // arc thickness px
    uint32_t warn_color;

    // Needle
    int      needle_width;
    int      needle_tail;       // negative px (tail behind pivot)
    uint32_t needle_color;

    // DSEG7 digital readout
    int      font_size;         // 48 / 55 / 65 / 75
    uint32_t digit_color;
    int      digit_y;           // y offset from screen center

    // Unit label below digits
    const char *unit_label;
    int         unit_font_size; // montserrat 14/16/20/30
    uint32_t    unit_color;
    int         unit_y;
} arc_cfg_t;

// ── Dual bar+digital config ──────────────────────────────────────
typedef struct {
    int      pid_left;
    int      pid_right;
    uint32_t color_left;
    uint32_t color_right;
    uint32_t warn_color;
    int      font_size;         // DSEG7 size for values
} dual_cfg_t;

// ── Single digital config ────────────────────────────────────────
typedef struct {
    int      pid_index;
    int      font_size;
    uint32_t value_color;
    uint32_t warn_color;
} digital_cfg_t;

// ── Page definition ──────────────────────────────────────────────
typedef struct {
    gauge_type_t type;
    union {
        arc_cfg_t     arc;
        dual_cfg_t    dual;
        digital_cfg_t digital;
    };
} gauge_page_def_t;

// ════════════════════════════════════════════════════════════════
//  PAGE DEFINITIONS — edit below to add / change pages
// ════════════════════════════════════════════════════════════════

// Page 0: RPM arc gauge — matches original design exactly
#define CFG_RPM { \
    .type = GAUGE_TYPE_ARC, \
    .arc = { \
        .val_min        = 0,        \
        .val_max        = 6500,     \
        .angle_range    = 280,      \
        .angle_start    = 130,      \
        .minor_count    = 66,       \
        .minor_width    = 2,        \
        .minor_len      = 10,       \
        .minor_color    = 0x3C78B4, \
        .major_nth      = 5,        \
        .major_width    = 4,        \
        .major_len      = 20,       \
        .major_color    = 0xA0D2FF, \
        .show_labels    = false,    \
        .warn_start     = 5250,     \
        .warn_end       = 6500,     \
        .warn_arc_width = 12,       \
        .warn_color     = 0xDC1E1E, \
        .needle_width   = 3,        \
        .needle_tail    = -20,      \
        .needle_color   = 0xFFFFFF, \
        .font_size      = 65,       \
        .digit_color    = 0xBEF9FD, \
        .digit_y        = 95,       \
        .unit_label     = "RPM",    \
        .unit_font_size = 30,       \
        .unit_color     = 0x60C1E4, \
        .unit_y         = 180,      \
    } \
}

// Page 1: Dual water + oil temp
#define CFG_DUAL_TEMP { \
    .type = GAUGE_TYPE_DUAL, \
    .dual = { \
        .pid_left    = 2,        \
        .pid_right   = 3,        \
        .color_left  = 0x4488FF, \
        .color_right = 0xFF8844, \
        .warn_color  = 0xFF3333, \
        .font_size   = 55,       \
    } \
}

// Page 2: Raw scrollable list
#define CFG_RAW { .type = GAUGE_TYPE_RAW }

// ── Master page array ────────────────────────────────────────────
#define GAUGE_PAGE_COUNT 3

static const gauge_page_def_t GAUGE_PAGES[GAUGE_PAGE_COUNT] = {
    CFG_RPM,
    CFG_DUAL_TEMP,
    CFG_RAW,
};
