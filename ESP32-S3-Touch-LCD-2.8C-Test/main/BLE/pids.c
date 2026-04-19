#include "pids.h"
#include <string.h>

// ── Combined PID list (built at runtime) ─────────────────────────
PIDDef PIDS[MAX_PIDS];
int    PID_COUNT = 0;
bool   pidEnabled[MAX_PIDS];

void buildPIDList(void) {
    PID_COUNT = 0;

    for (int i = 0; i < STANDARD_PID_COUNT && PID_COUNT < MAX_PIDS; i++)
        PIDS[PID_COUNT++] = STANDARD_PIDS[i];

    for (int i = 0; i < CUSTOM_PID_COUNT && PID_COUNT < MAX_PIDS; i++)
        PIDS[PID_COUNT++] = CUSTOM_PIDS[i];

    for (int i = 0; i < PID_COUNT; i++)
        pidEnabled[i] = true;
}
