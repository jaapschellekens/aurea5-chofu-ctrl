#pragma once
#include <Arduino.h>

// ═══════════════════════════════════════════════════════════════
//  HARDWARE / BOARD INSTELLINGEN
// ═══════════════════════════════════════════════════════════════

#define USE_LCD        true
#define USE_LED_MATRIX true   // alleen effectief op UNO R4 WiFi

#if defined(ARDUINO_UNOR4_WIFI)
  // UNO R4 WiFi: EEPROM schrijft direct, geen commit nodig
  #define EEPROM_BEGIN()
  #define EEPROM_COMMIT()
#else
  // ESP32: EEPROM is flash-emulatie, vereist begin() en commit()
  #define EEPROM_SIZE    64
  #define EEPROM_BEGIN() EEPROM.begin(EEPROM_SIZE)
  #define EEPROM_COMMIT() EEPROM.commit()
  // Chofu UART pins — pas aan voor jouw ESP32 board
  #define CHOFU_RX_PIN   16
  #define CHOFU_TX_PIN   17
#endif

// ═══════════════════════════════════════════════════════════════
//  MODUS
// ═══════════════════════════════════════════════════════════════

enum class Modus { AUTO, FF_AUTO, WATER, FF_WATER, HANDMATIG };

inline const char* modus_naar_str(Modus m){
  switch(m){
    case Modus::FF_AUTO:   return "ff_auto";
    case Modus::WATER:     return "water";
    case Modus::FF_WATER:  return "ff_water";
    case Modus::HANDMATIG: return "handmatig";
    default:               return "auto";
  }
}

inline Modus str_naar_modus(const String& s){
  if(s == "ff_auto")   return Modus::FF_AUTO;
  if(s == "water")     return Modus::WATER;
  if(s == "ff_water")  return Modus::FF_WATER;
  if(s == "handmatig") return Modus::HANDMATIG;
  return Modus::AUTO;
}

// ═══════════════════════════════════════════════════════════════
//  CONTROLLER TOESTAND
// ═══════════════════════════════════════════════════════════════

struct ControllerState {
  uint8_t  stand             = 0;
  bool     wp_aan            = false;
  float    pid_integraal     = 0;
  float    pid_vorige_fout   = 0;
  float    pid_output        = 0;
  float    ff_integraal      = 0;
  uint32_t vorige_stand_wijz_ms = 0;

  // WP volledig uitzetten + alle integralen wissen
  void zet_uit() {
    stand = 0; wp_aan = false;
    pid_integraal = 0; pid_vorige_fout = 0; ff_integraal = 0;
  }
  void koude_start(uint32_t nu) {
    zet_uit();
    // 700s in het verleden: altijd groter dan de grootste hysteresis (HYST_SLOW=600s),
    // ook bij uint32_t-wraparound vlak na boot.
    vorige_stand_wijz_ms = nu - 700000UL;
  }
  // Alleen PID integralen wissen (bijv. bij modus-wissel zonder afsluiten)
  void reset_pid() { pid_integraal = 0; pid_vorige_fout = 0; }
  void reset_ff()  { ff_integraal = 0; }
};

// ═══════════════════════════════════════════════════════════════
//  FF CONSTANTEN (niet instelbaar via MQTT, kalibratie-output)
// ═══════════════════════════════════════════════════════════════

const float FF_LEARN_RATE  = 0.0002f;  // tijdconstante ~7 uur
const float FF_KI_AUTO     = 0.026f;   // integraalversterking auto  — geoptimaliseerd KGE
const float FF_KI_WATER    = 0.017f;   // integraalversterking water — geoptimaliseerd KGE
const float FF_COAST_AUTO  = 0.54f;    // anticipatiezone auto  [°C] — geoptimaliseerd KGE
const float FF_COAST_WATER = 4.76f;    // anticipatiezone water [°C] — geoptimaliseerd KGE
const float AUTO_AAN_DREMPEL =  0.4f;  // kamer °C onder setpoint → WP aan
const float AUTO_UIT_DREMPEL = -0.4f;  // kamer °C boven setpoint → WP afbouwen

// Vermogen per stand 0–12 (W)
const int VERMOGEN[] = {0, 240, 420, 640, 850, 1050, 1250, 1450, 1550, 1650, 1700, 1750, 1800};
