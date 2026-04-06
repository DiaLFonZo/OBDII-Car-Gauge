#pragma once
#include <Arduino.h>

void initDisplay();
void showBoot();
void showAutoConnect();
void showConnecting();
void showScanning(int deviceCount);
void showPIDPage(int pidIndex, bool locked);
void updateDisplay(int state);
void VextON();

// Virtual device list helpers (used by input.cpp)
int  getVirtualCount();
bool isForgetSlot(int index);
