#pragma once

// ═══════════════════════════════════════════════════════════════
//  pids.h — 2025 Chevrolet Silverado 1500 ZR2 / LZ0 3.0 Duramax
// ═══════════════════════════════════════════════════════════════
//
//  Background probe has been REMOVED.
//  All PIDs in this list are polled immediately after connect.
//  All PIDs are activated by default on first run.
//
// ── HOW TO ADD / EDIT A PID ─────────────────────────────────────
//  { "CMD", "NAME", "unit", min, max, warn, "formula", isBoolean, regenFlag, skip }
//
//  warn:  value where arc turns red. Set to 0 for no warn zone.
//         Example: RPM warn=5250 means arc goes green→yellow→red at 5250
//
//  skip:  0 = poll every cycle, 1 = every 2nd, etc.
//
// ── CONFIRMED GM / DURAMAX PIDs ─────────────────────────────────
//
//  221154   Engine Oil Temp     formula: A-40    (°C, confirmed GM)
//  221940   Trans Fluid Temp    formula: A-40    (°C, confirmed GM, header 7E2)
//  0105     Coolant Temp        formula: A-40    (SAE standard)
//  010F     Intake Air Temp     formula: A-40    (SAE standard, pre-intercooler)
//
// ── LZ0-SPECIFIC PIDs — PLACEHOLDERS ────────────────────────────
//  The hex addresses below are UNCONFIRMED for the LZ0 (Global B).
//  The LZ0 uses a different ECM architecture than the LM2 (Global A).
//  Proprietary tools (gretio/BiScan/Banks iDash) do not yet publicly
//  document LZ0 mode-22 hex addresses for emissions PIDs.
//
//  TO FIND YOUR ACTUAL PIDs:
//    1. Open Car Scanner Pro on Android
//    2. Connect to your truck
//    3. Go to the gauge that IS working (e.g. DPF Soot)
//    4. Tap the gauge → Settings → Info → note the "OBD command"
//    5. Replace the "221XXX" placeholder below with that hex value
//
//  Placeholders are marked with  ← PLACEHOLDER comments.
//  They will return NO DATA until you fill in the real address.
//
// ═══════════════════════════════════════════════════════════════

struct PIDDef {
  const char* cmd;
  const char* name;
  const char* unit;
  float       valMin;
  float       valMax;
  float       warn;    // value at which arc turns red (0 = no warn zone)
  const char* formula;
  bool        isBoolean;
  bool        regenFlag;
  uint8_t     skip;
};

// ── YOUR PID LIST ────────────────────────────────────────────────
// Order here = order shown on gauge pages
static const PIDDef STANDARD_PIDS[] = {

  // cmd        name           unit    min    max    warn   formula          bool    regen  skip
  { "010C",  "RPM",          "rpm",    0,   6000,  5250,  "(A*256+B)/4",   false,  false,   0 },
  { "0105",  "WATER TEMP",   "deg.C", -40,   120,   105,  "A-40",          false,  false,  10 },
  { "221154","OIL TEMP",     "deg.C", -40,   150,   130,  "A-40",          false,  false,  10 },
  { "221940","TRANS TEMP",   "deg.C", -40,   120,   105,  "A-40",          false,  false,  10 },
  { "018B",  "REGEN",        "",        0,     1,     0,  "B-2",           true,   true,  100 },
  { "018B",  "SOOT",         "%",       0,   100,    80,  "(100/255)*C",   false,  false, 100 },

  // ── EGT placeholder — fill in address when found ────────────────
  // { "221XXX", "EGT", "deg.C", 0, 900, 700, "(A*256+B)/10-40", false, false, 1 },

};
static const int STANDARD_PID_COUNT = sizeof(STANDARD_PIDS) / sizeof(STANDARD_PIDS[0]);

// ── CUSTOM PIDs — add extras here after you confirm addresses ────
static const PIDDef CUSTOM_PIDS[] = {
  // Example once you find the address:
  // { "221A2F", "DEF LEVEL",  "%",  0, 100, "A/2.55", false, false, 3 },
};
static const int CUSTOM_PID_COUNT = sizeof(CUSTOM_PIDS) / sizeof(CUSTOM_PIDS[0]);

// ── COMBINED LIST — built at runtime in obd.cpp ──────────────────
#define MAX_PIDS 64
extern PIDDef PIDS[MAX_PIDS];
extern int    PID_COUNT;
void buildPIDList();
