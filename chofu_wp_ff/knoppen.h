#pragma once
#include "globals.h"

#define BTN_UP   5   // D5 — stand omhoog
#define BTN_DOWN 6   // D6 — stand omlaag

// Verbind knoppen met GND (geen externe weerstand nodig, INPUT_PULLUP actief).

void knoppen_init();
void check_knoppen();
