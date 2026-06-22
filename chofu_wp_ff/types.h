#ifndef CHOFU_TYPES_H
#define CHOFU_TYPES_H
#include <Arduino.h>

// ═══════════════════════════════════════════════════════════════
//  MQTT TOPIC-PREFIX / HOME ASSISTANT DEVICE (compile-time)
// ═══════════════════════════════════════════════════════════════
// Pas deze drie aan voor een TEST-opstelling op een tweede Arduino, zodat het
// productiesysteem niet overschreven wordt. Alle MQTT-topics en de HA-discovery
// (device-id, unique_id's, discovery-node) gebruiken deze waarden.
//
// Productie : MQTT_PREFIX "chofu"      HA_NODE "chofu_hp"      HA_DEV_NAME "Chofu WP"
// Test  bv. : MQTT_PREFIX "chofu_test" HA_NODE "chofu_test_hp" HA_DEV_NAME "Chofu WP TEST"
//
// Let op: hier wijzigen (niet in config.h) — config.h wordt niet door alle
// .cpp-bestanden gezien. Kan ook globaal via build-flag: -DMQTT_PREFIX="\"...\"".
#ifndef MQTT_PREFIX
  #define MQTT_PREFIX  "chofu"
#endif
#ifndef HA_NODE
  #define HA_NODE      "chofu hp"
#endif
#ifndef HA_DEV_NAME
  #define HA_DEV_NAME  "Chofu WP"
#endif

// ═══════════════════════════════════════════════════════════════
//  HARDWARE / BOARD INSTELLINGEN
// ═══════════════════════════════════════════════════════════════

#if defined(ARDUINO_UNOR4_WIFI)
  #define USE_LCD        true
#else
  #define USE_LCD        false  // geen LCD op ESP32 — zet op true als LCD aangesloten is
#endif
#define USE_LED_MATRIX true   // alleen effectief op UNO R4 WiFi

#if defined(ARDUINO_UNOR4_WIFI)
  // UNO R4 WiFi: EEPROM schrijft direct, geen commit nodig
  #define EEPROM_BEGIN()
  #define EEPROM_COMMIT()
#else
  // ESP32: gebruikt Preferences (NVS) — geen EEPROM.begin() nodig
  #define EEPROM_BEGIN()
  #define EEPROM_COMMIT()
  // Chofu UART pins — pas aan voor jouw ESP32 board
  #define CHOFU_RX_PIN   16
  #define CHOFU_TX_PIN   17
#endif

// ── SWW (tapwater) driewegklep-relais ───────────────────────────
// GPIO die het relais voor de driewegklep naar het tapwatervat schakelt.
// Pas hier aan (of via build-flag) voor jouw bedrading — niet in config.h
// (dat bestand wordt niet door regelaar.cpp gezien).
#ifndef SWW_KLEP_PIN
  #if defined(ARDUINO_UNOR4_WIFI)
    #define SWW_KLEP_PIN 2     // D2 (vrij op UNO R4)
  #else
    #define SWW_KLEP_PIN 25    // GPIO25 (veilig op ESP32)
  #endif
#endif
#ifndef SWW_KLEP_ACTIEF_HOOG
  #define SWW_KLEP_ACTIEF_HOOG 1   // 1 = HIGH opent klep naar tapwatervat
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
  uint32_t ff_water_koel_start_ms = 0; // soft-start timer na 0 -> 1 in FF_WATER-koeling
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
    ff_water_koel_start_ms = 0;
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
#endif // CHOFU_TYPES_H
