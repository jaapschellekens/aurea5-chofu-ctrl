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
  #define EEPROM_SIZE    80
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
  uint32_t wp_start_ms = 0;          // tijdstip waarop WP voor het laatste aansloeg
  uint32_t wp_uit_ms   = 0;          // tijdstip waarop WP voor het laatste uitschakelde
  bool     wp_thermal_stop = false;  // true = laatste stop was thermisch (overshoot), false = seizoensmatig

  // Ring-buffer voor predictieve terugschakeling (sliding-window afgeleide kamertemp)
  // 21 slots × 60s sample-interval = 20 min venster (past bij τ_huis ≈ 13 uur)
  static const uint8_t DERIV_BUF_N = 21;
  float    deriv_buf[DERIV_BUF_N]  = {};  // kamertemp (°C) ringbuffer
  uint8_t  deriv_idx                = 0;   // schrijfindex
  uint8_t  deriv_count              = 0;   // aantal gevulde slots (0..DERIV_BUF_N)
  uint32_t deriv_last_ms            = 0;   // tijdstip laatste sample

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
  // Zacht reset: halveer integraal zodat herstart minder overreageert (gebruikt in AUTO)
  void soft_reset_pid() { pid_integraal *= 0.5f; pid_vorige_fout = 0; }
  void reset_ff()  { ff_integraal = 0; }
};

// ═══════════════════════════════════════════════════════════════
//  FF CONSTANTEN (niet instelbaar via MQTT, kalibratie-output)
// ═══════════════════════════════════════════════════════════════

const float FF_LEARN_RATE  = 0.0002f;  // tijdconstante ~7 uur
const float FF_KI_AUTO     = 0.026f;   // integraalversterking auto  — geoptimaliseerd KGE
const float FF_KI_WATER    = 0.017f;   // integraalversterking water — geoptimaliseerd KGE
const float FF_COAST_AUTO  = 0.30f;    // anticipatiezone auto  [°C] — verlaagd na sim-analyse (was 0.54)
const float FF_COAST_WATER = 2.5f;     // anticipatiezone water [°C] — verlaagd na sim-analyse (was 4.76)
const float AUTO_AAN_DREMPEL      =  0.4f;    // kamer °C onder setpoint → WP aan
const float AUTO_UIT_DREMPEL      = -0.4f;    // kamer °C boven setpoint → WP afbouwen
const float AUTO_AANVOER_DEADBAND =  0.5f;    // °C aanvoerfout: geen standwijziging binnen deadband

// FF_AUTO start-beperking — als globals zodat ze via MQTT instelbaar zijn voor HIL-testen
// Zie globals.h/cpp voor FF_MIN_OFF_MS, FF_RESTART_COAST, AUTO_HYST_DOWN_MS

// Vermogen per stand 0–12 (W)
const int VERMOGEN[] = {0, 240, 420, 640, 850, 1050, 1250, 1450, 1550, 1650, 1700, 1750, 1800};
