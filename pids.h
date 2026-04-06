#pragma once

// ═══════════════════════════════════════════════════════════════
//  pids.h  —  Define all your gauges here.
//             Add a row to PIDS[] and you're done.
// ═══════════════════════════════════════════════════════════════
//
// HOW TO ADD A PID — 3 steps
// ──────────────────────────
// 1. Find your PID info (from Torque app, OBD wiki, etc.)
//    You need: command, equation, min, max, unit.
//
// 2. Pick the right EQ_ type from the list below and figure
//    out the offset if needed.
//
// 3. Add one line to PIDS[] at the bottom. Done.
//
//
// EQUATION TYPES
// ──────────────
// Torque shows equations like:  A, A-40, (A*256+B)/4, etc.
// Match your Torque equation to the EQ_ type below:
//
//   Torque equation        → EQ_ type        offset
//   ─────────────────────────────────────────────────
//   (A*256+B)/4            → EQ_RPM           0
//   A                      → EQ_A             0
//   A-40                   → EQ_A_MINUS_40    0
//   A-50  (or A+20, etc.)  → EQ_A_OFFSET     -50  (put your number here)
//   B-2  (boolean 0/1)     → EQ_B_BOOL        0
//   (100/255)*C            → EQ_C_PERCENT      0
//   F  (manufacturer PID)  → EQ_CUSTOM_FF      0  (add offset if needed)
//
//
// WORKED EXAMPLE — Coolant Temp from Torque:
//   Command:  0105
//   Equation: A-40
//   Min: -40    Max: 215    Unit: deg.C
//
//   → Add this line to PIDS[]:
//   { "0105", "COOLANT", "deg.C", -40, 215, EQ_A_MINUS_40, 0, false, false },
//
//
// WORKED EXAMPLE — Exhaust Temp (manufacturer PID):
//   Command:  FF1285
//   Equation: F  (F is just byte A with an offset of -50)
//   Min: -50    Max: 650    Unit: deg.C
//
//   → Add this line to PIDS[]:
//   { "FF1285", "EXH TEMP", "deg.C", -50, 650, EQ_CUSTOM_FF, -50, false, false },
//
//
// FIELD REFERENCE
// ───────────────
//  cmd        Command string sent to ELM327, e.g. "010C"
//  name       Page title shown on display  (max ~13 chars)
//  unit       Label shown under value      (e.g. "rpm", "" for ON/OFF)
//  valMin     Progress bar lower bound
//  valMax     Progress bar upper bound
//  equation   EQ_ type — see table above
//  offset     Only used by EQ_A_OFFSET and EQ_CUSTOM_FF
//  isBoolean  true  → displays ON / OFF  (no progress bar)
//             false → displays number + progress bar
//  regenFlag  true  → when this PID is non-zero, flash border on ALL pages
//             (set on at most one PID)
// ═══════════════════════════════════════════════════════════════

enum PIDEquation {
  EQ_RPM,           // (A*256+B)/4
  EQ_A,             // A
  EQ_A_MINUS_40,    // A-40
  EQ_A_OFFSET,      // A + offset
  EQ_B_BOOL,        // (B-2) != 0
  EQ_C_PERCENT,     // (100/255)*C
  EQ_CUSTOM_FF,     // A + offset  (for manufacturer / mode FF PIDs)
};

struct PIDDef {
  const char* cmd;
  const char* name;
  const char* unit;
  float       valMin;
  float       valMax;
  PIDEquation equation;
  float       offset;
  bool        isBoolean;
  bool        regenFlag;
};

// ── ADD YOUR PIDS HERE ──────────────────────────────────────────
//   cmd        name            unit     min    max      equation       offset  bool   regen
static const PIDDef PIDS[] = {
  { "010C",   "RPM",          "rpm",    0,    8000,  EQ_RPM,          0,     false, false },
  { "010D",   "SPEED",        "km/h",   0,     200,  EQ_A,            0,     false, false },
  { "018B",   "REGEN",        "",       0,       1,  EQ_B_BOOL,       0,     true,  true  },
  { "018B",   "SOOT LEVEL",   "%",      0,     100,  EQ_C_PERCENT,    0,     false, false },

  // ── Examples — uncomment to add ─────────────────────────────
  // { "0105", "COOLANT",    "deg.C", -40,  215, EQ_A_MINUS_40,   0, false, false },
  // { "0111", "THROTTLE",   "%",       0,  100, EQ_A_OFFSET,     0, false, false },
  // { "0104", "ENG LOAD",   "%",       0,  100, EQ_A_OFFSET,     0, false, false },
  // { "010F", "INTAKE AIR", "deg.C", -40,  215, EQ_A_MINUS_40,   0, false, false },
  // { "0146", "AMB TEMP",   "deg.C", -40,  215, EQ_A_MINUS_40,   0, false, false },
};

static const int PID_COUNT = sizeof(PIDS) / sizeof(PIDS[0]);
