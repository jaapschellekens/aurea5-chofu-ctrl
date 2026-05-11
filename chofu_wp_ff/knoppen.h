#ifndef CHOFU_KNOPPEN_H
#define CHOFU_KNOPPEN_H
#include "globals.h"

// GPIO6-11 zijn SPI-flash pins op ESP32 en mogen NIET als IO gebruikt worden.
#if defined(ARDUINO_UNOR4_WIFI)
  #define BTN_UP   5   // D5 — stand omhoog
  #define BTN_DOWN 6   // D6 — stand omlaag
#else
  #define BTN_UP   4   // GPIO4 — stand omhoog  (veilig op ESP32)
  #define BTN_DOWN 13  // GPIO13 — stand omlaag (veilig op ESP32)
#endif

// Verbind knoppen met GND (geen externe weerstand nodig, INPUT_PULLUP actief).

void knoppen_init();
void check_knoppen();
#endif // CHOFU_KNOPPEN_H
