#pragma once
#include <Arduino.h>
#include "bt.h"

void initInput();
void handleButton();
void handlePageAdvance();
void resetGaugePage();

extern int  gaugePage;
extern bool gaugeLocked;
extern int  pidSelectorCursor;
extern int  menuCursor;
