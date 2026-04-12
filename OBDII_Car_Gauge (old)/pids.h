#pragma once

// ═══════════════════════════════════════════════════════════════
//  pids.h — 2025 Chevrolet Silverado 1500 ZR2 / LZ0 3.0 Duramax
// ═══════════════════════════════════════════════════════════════
//
//  { "CMD", "NAME", "unit", min, max, warn, "formula", isBoolean, regenFlag, skip }
//
//  warn:  value where bar turns red. 0 = no warn zone.
//  skip:  0 = poll every cycle, 10 = every 11th cycle, etc.
//
// ── CONFIRMED PIDs ──────────────────────────────────────────────
//  010C  RPM               (A*256+B)/4
//  0105  Coolant Temp      A-40  °C
//  010D  Vehicle Speed     A  km/h
//  010F  Intake Air Temp   A-40  °C
//  0104  Engine Load       (A*100)/255  %
//  010E  Timing Advance    (A/2)-64  deg
//  0133  Baro Pressure     A  kPa
//  0142  Ctrl Module Volt  (A*256+B)/1000  V
//  221154  Oil Temp        A-40  °C  (GM confirmed)
//  221940  Trans Temp      A-40  °C  (GM confirmed, header 7E2)
//  018B  REGEN / SOOT      GM DPF status
//
// ── UNCONFIRMED LZ0 PIDs ────────────────────────────────────────
//  015B  Hybrid Battery — will not respond on Duramax, remove if noisy
//  EGT  — address unknown, placeholder commented out
// ═══════════════════════════════════════════════════════════════

struct PIDDef {
  const char* cmd;
  const char* name;
  const char* unit;
  float       valMin;
  float       valMax;
  float       warn;
  const char* formula;
  bool        isBoolean;
  bool        regenFlag;
  uint8_t     skip;
};

// ── PID LIST — order = gauge page order ──────────────────────────
static const PIDDef STANDARD_PIDS[] = {

  // cmd        name           unit      min    max    warn   formula            bool    regen  skip
  { "010C",   "RPM",          "rpm",      0,   6000,  5250,  "(A*256+B)/4",     false,  false,   0 },
  { "010D",   "SPEED",        "km/h",     0,    250,   200,  "A",               false,  false,   0 },
  { "0105",   "WATER TEMP",   "deg.C",  -40,    120,   105,  "A-40",            false,  false,  10 },
  { "221154", "OIL TEMP",     "deg.C",  -40,    150,   130,  "A-40",            false,  false,  10 },
  { "221940", "TRANS TEMP",   "deg.C",  -40,    120,   105,  "A-40",            false,  false,  10 },
  { "010F",   "INTAKE TEMP",  "deg.C",  -40,    120,   100,  "A-40",            false,  false,  10 },
  { "0104",   "ENG LOAD",     "%",        0,    100,    80,  "(A*100)/255",     false,  false,   0 },
  { "010B",   "MAP",          "kPa",      0,    255,   200,  "A",               false,  false,   0 },
  { "010E",   "TIMING ADV",   "deg",    -64,     64,     0,  "(A/2)-64",        false,  false,   5 },
  { "0133",   "BARO PRESS",   "kPa",     50,    110,     0,  "A",               false,  false,  30 },
  { "0142",   "CTRL VOLT",    "V",        0,     16,    14,  "(A*256+B)/1000",  false,  false,  20 },
  { "018B",   "REGEN",        "",         0,      1,     0,  "B-2",             true,   true,  100 },
  { "018B",   "SOOT",         "%",        0,    100,    80,  "(100/255)*C",     false,  false, 100 },

  // ── Remove or replace once LZ0 addresses confirmed ──────────────
  // { "015B",  "HYB BATT",  "%",  0, 100, 20, "A*100/255", false, false, 30 },  // won't respond on diesel
  // { "221XXX","EGT",       "deg.C", 0, 900, 700, "(A*256+B)/10-40", false, false, 1 },

};
static const int STANDARD_PID_COUNT = sizeof(STANDARD_PIDS) / sizeof(STANDARD_PIDS[0]);

// ── CUSTOM PIDs — confirmed LZ0 addresses go here ────────────────
static const PIDDef CUSTOM_PIDS[] = {
  // { "221A2F", "DEF LEVEL", "%", 0, 100, 15, "A/2.55", false, false, 30 },
};
static const int CUSTOM_PID_COUNT = sizeof(CUSTOM_PIDS) / sizeof(CUSTOM_PIDS[0]);

// ── COMBINED LIST — built at runtime in obd.cpp ──────────────────
#define MAX_PIDS 64
extern PIDDef PIDS[MAX_PIDS];
extern int    PID_COUNT;
void buildPIDList();
