#pragma once

#include <stdint.h>
#include <stdbool.h>

// ═══════════════════════════════════════════════════════════════
//  pids.h — 2025 Chevrolet Silverado 1500 ZR2 / LZ0 3.0 Duramax
// ═══════════════════════════════════════════════════════════════

typedef struct {
    const char *cmd;
    const char *name;
    const char *unit;
    float       valMin;
    float       valMax;
    float       warn;
    const char *formula;
    bool        isBoolean;
    bool        regenFlag;
    uint8_t     skip;
} PIDDef;

// ── Standard PID list ────────────────────────────────────────────
static const PIDDef STANDARD_PIDS[] = {
  { "010C",   "RPM",          "rpm",      0,   6000,  5250,  "(A*256+B)/4",     false, false,   0 },
  { "010D",   "SPEED",        "km/h",     0,    250,   200,  "A",               false, false,   0 },
  { "0105",   "WATER TEMP",   "deg.C",  -40,    120,   105,  "A-40",            false, false,  10 },
  { "221154", "OIL TEMP",     "deg.C",  -40,    150,   130,  "A-40",            false, false,  10 },
  { "221940", "TRANS TEMP",   "deg.C",  -40,    120,   105,  "A-40",            false, false,  10 },
  { "010F",   "INTAKE TEMP",  "deg.C",  -40,    120,   100,  "A-40",            false, false,  10 },
  { "0104",   "ENG LOAD",     "%",        0,    100,    80,  "(A*100)/255",     false, false,   0 },
  { "010B",   "MAP",          "kPa",      0,    255,   200,  "A",               false, false,   0 },
  { "010E",   "TIMING ADV",   "deg",    -64,     64,     0,  "(A/2)-64",        false, false,   5 },
  { "0133",   "BARO PRESS",   "kPa",     50,    110,     0,  "A",               false, false,  30 },
  { "0142",   "CTRL VOLT",    "V",        0,     16,    14,  "(A*256+B)/1000",  false, false,  20 },
  { "018B",   "REGEN",        "",         0,      1,     0,  "B-2",             true,  true,  100 },
  { "018B",   "SOOT",         "%",        0,    100,    80,  "(100/255)*C",     false, false, 100 },
};

#define STANDARD_PID_COUNT  ((int)(sizeof(STANDARD_PIDS) / sizeof(STANDARD_PIDS[0])))

// ── Custom PIDs (add confirmed LZ0 addresses here) ───────────────
static const PIDDef CUSTOM_PIDS[] = {
    // { "221A2F", "DEF LEVEL", "%", 0, 100, 15, "A/2.55", false, false, 30 },
};

#define CUSTOM_PID_COUNT  ((int)(sizeof(CUSTOM_PIDS) / sizeof(CUSTOM_PIDS[0])))

// ── Combined runtime list (built by buildPIDList) ────────────────
#define MAX_PIDS 64

extern PIDDef PIDS[MAX_PIDS];
extern int    PID_COUNT;
extern bool   pidEnabled[MAX_PIDS];

void buildPIDList(void);
