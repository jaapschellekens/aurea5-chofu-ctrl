#include "eeprom.h"

// ═══════════════════════════════════════════════════════════════
//  UNO R4 WiFi — EEPROM emulatie
// ═══════════════════════════════════════════════════════════════
#if defined(ARDUINO_UNOR4_WIFI)

void eeprom_init(){
  if(EEPROM.read(ADDR_MAGIC) != EEPROM_MAGIC){
    Serial.println("EEPROM: eerste keer - schrijf defaults");
    eeprom_save();
  } else {
    Serial.println("EEPROM: lees opgeslagen settings");
    eeprom_load();
  }
}

void eeprom_save(){
  EEPROM.write(ADDR_MAGIC, EEPROM_MAGIC);
  EEPROM.put(ADDR_SETPOINT, setpoint);
  EEPROM.put(ADDR_KP, Kp);
  EEPROM.put(ADDR_KI, Ki);
  EEPROM.put(ADDR_KD, Kd);
  EEPROM.put(ADDR_STOOKLIJN_GRENS, STOOKLIJN_GRENS);
  EEPROM.put(ADDR_STOOKLIJN_FACTOR, STOOKLIJN_FACTOR);
  EEPROM.put(ADDR_T_VORST, T_VORST);
  EEPROM.put(ADDR_SUPPLY_MAX, SUPPLY_MAX);
  EEPROM.put(ADDR_KOELING_MIN_BUITEN, KOELING_MIN_BUITEN);
  EEPROM.put(ADDR_STOOKLIJN_UIT, STOOKLIJN_UIT_GRENS);
  EEPROM.put(ADDR_FF_UA_HOUSE, ff_UA_house);
  EEPROM.put(ADDR_FF_UA_EMITTER, ff_UA_emitter);
  EEPROM.write(ADDR_MODUS, (uint8_t)modus);
  EEPROM.put(ADDR_KP_WATER, Kp_water);
  EEPROM.put(ADDR_KI_WATER, Ki_water);
  EEPROM.put(ADDR_KD_WATER, Kd_water);
  EEPROM.put(ADDR_STOOKLIJN_AAN, STOOKLIJN_AAN_GRENS);
  EEPROM.put(ADDR_SUPPLY_MIN, SUPPLY_MIN);
  EEPROM.put(ADDR_WATER_SP_MIN, WATER_SP_MIN);
  EEPROM.write(ADDR_MAX_STAND, MAX_STAND);
  EEPROM.put(ADDR_SWW_SETPOINT, SWW_SETPOINT);
  EEPROM.write(ADDR_SWW_MAX_STAND, SWW_MAX_STAND);
  EEPROM.write(ADDR_KAMER_IN_WATER, kamer_in_water ? 1 : 0);
  EEPROM.put(ADDR_KOEL_DEADBAND, KOEL_DEADBAND);
  Serial.println("EEPROM: settings opgeslagen");
}

void eeprom_load(){
  EEPROM.get(ADDR_SETPOINT, setpoint);
  EEPROM.get(ADDR_KP, Kp);
  EEPROM.get(ADDR_KI, Ki);
  EEPROM.get(ADDR_KD, Kd);
  EEPROM.get(ADDR_STOOKLIJN_GRENS, STOOKLIJN_GRENS);
  EEPROM.get(ADDR_STOOKLIJN_FACTOR, STOOKLIJN_FACTOR);
  EEPROM.get(ADDR_T_VORST, T_VORST);
  if(T_VORST < -10 || T_VORST > 10) T_VORST = 4.0;
  EEPROM.get(ADDR_SUPPLY_MAX, SUPPLY_MAX);
  if(SUPPLY_MAX < 40 || SUPPLY_MAX > 80) SUPPLY_MAX = 60.0;
  EEPROM.get(ADDR_KOELING_MIN_BUITEN, KOELING_MIN_BUITEN);
  if(KOELING_MIN_BUITEN < 0 || KOELING_MIN_BUITEN > 30) KOELING_MIN_BUITEN = 18.0;
  EEPROM.get(ADDR_STOOKLIJN_UIT, STOOKLIJN_UIT_GRENS);
  if(STOOKLIJN_UIT_GRENS < 5 || STOOKLIJN_UIT_GRENS > 30) STOOKLIJN_UIT_GRENS = 15.0;
  EEPROM.get(ADDR_FF_UA_HOUSE, ff_UA_house);
  if(isnan(ff_UA_house) || ff_UA_house < 50 || ff_UA_house > 500) ff_UA_house = 272.5;
  EEPROM.get(ADDR_FF_UA_EMITTER, ff_UA_emitter);
  if(isnan(ff_UA_emitter) || ff_UA_emitter < 50 || ff_UA_emitter > 500) ff_UA_emitter = 267.5;
  uint8_t opgeslagen_modus = EEPROM.read(ADDR_MODUS);
  modus = (opgeslagen_modus <= (uint8_t)Modus::FF_WATER && opgeslagen_modus != (uint8_t)Modus::HANDMATIG)
          ? (Modus)opgeslagen_modus : Modus::AUTO;
  EEPROM.get(ADDR_KP_WATER, Kp_water);
  if(isnan(Kp_water) || Kp_water < 0.1f || Kp_water > 500.0f) Kp_water = 50.0f;
  EEPROM.get(ADDR_KI_WATER, Ki_water);
  if(isnan(Ki_water) || Ki_water < 0.0f || Ki_water > 5.0f)   Ki_water = 0.800f;
  EEPROM.get(ADDR_KD_WATER, Kd_water);
  if(isnan(Kd_water) || Kd_water < 0.0f || Kd_water > 50.0f)  Kd_water = 0.010f;
  EEPROM.get(ADDR_STOOKLIJN_AAN, STOOKLIJN_AAN_GRENS);
  if(STOOKLIJN_AAN_GRENS < 0 || STOOKLIJN_AAN_GRENS > 25) STOOKLIJN_AAN_GRENS = 13.0f;
  EEPROM.get(ADDR_SUPPLY_MIN, SUPPLY_MIN);
  if(isnan(SUPPLY_MIN) || SUPPLY_MIN < 10 || SUPPLY_MIN > 25) SUPPLY_MIN = 17.0f;
  EEPROM.get(ADDR_WATER_SP_MIN, WATER_SP_MIN);
  if(isnan(WATER_SP_MIN) || WATER_SP_MIN < 10 || WATER_SP_MIN > 30) WATER_SP_MIN = 16.0f;
  MAX_STAND = EEPROM.read(ADDR_MAX_STAND);
  if(MAX_STAND < 1 || MAX_STAND > 8) MAX_STAND = 8;
  EEPROM.get(ADDR_SWW_SETPOINT, SWW_SETPOINT);
  if(isnan(SWW_SETPOINT) || SWW_SETPOINT < 30 || SWW_SETPOINT > 60) SWW_SETPOINT = 50.0f;
  SWW_MAX_STAND = EEPROM.read(ADDR_SWW_MAX_STAND);
  if(SWW_MAX_STAND < 1 || SWW_MAX_STAND > 8) SWW_MAX_STAND = 8;
  kamer_in_water = (EEPROM.read(ADDR_KAMER_IN_WATER) != 0);
  EEPROM.get(ADDR_KOEL_DEADBAND, KOEL_DEADBAND);
  if(isnan(KOEL_DEADBAND) || KOEL_DEADBAND < 0 || KOEL_DEADBAND > 5) KOEL_DEADBAND = 1.0f;
  Serial.print("EEPROM: geladen - SP:"); Serial.print(setpoint,1);
  Serial.print(" PID:"); Serial.print(Kp,2); Serial.print("/"); Serial.print(Ki,3); Serial.print("/"); Serial.println(Kd,2);
  Serial.print("  FF UA huis:"); Serial.print(ff_UA_house,0);
  Serial.print(" emitter:"); Serial.println(ff_UA_emitter,0);
}

// ═══════════════════════════════════════════════════════════════
//  ESP32 — Preferences (NVS flash, ingebouwd in ESP32 core)
// ═══════════════════════════════════════════════════════════════
#else
#include <Preferences.h>
static Preferences prefs;

void eeprom_init(){
  prefs.begin("chofu", true);  // read-only check
  bool vers_ok = (prefs.getUChar("version", 0) == EEPROM_MAGIC);
  prefs.end();
  if(!vers_ok){
    Serial.println("NVS: eerste keer of nieuw versie - schrijf defaults");
    eeprom_save();
  } else {
    Serial.println("NVS: lees opgeslagen settings");
    eeprom_load();
  }
}

void eeprom_save(){
  prefs.begin("chofu", false);  // read-write
  prefs.putUChar("version",    EEPROM_MAGIC);
  prefs.putFloat("setpoint",   setpoint);
  prefs.putFloat("Kp",         Kp);
  prefs.putFloat("Ki",         Ki);
  prefs.putFloat("Kd",         Kd);
  prefs.putFloat("sl_grens",   STOOKLIJN_GRENS);
  prefs.putFloat("sl_factor",  STOOKLIJN_FACTOR);
  prefs.putFloat("t_vorst",    T_VORST);
  prefs.putFloat("supply_max", SUPPLY_MAX);
  prefs.putFloat("koel_min_b", KOELING_MIN_BUITEN);
  prefs.putFloat("sl_uit",     STOOKLIJN_UIT_GRENS);
  prefs.putFloat("ff_ua_huis", ff_UA_house);
  prefs.putFloat("ff_ua_emit", ff_UA_emitter);
  prefs.putUChar("modus",      (uint8_t)modus);
  prefs.putFloat("Kp_water",   Kp_water);
  prefs.putFloat("Ki_water",   Ki_water);
  prefs.putFloat("Kd_water",   Kd_water);
  prefs.putFloat("sl_aan",     STOOKLIJN_AAN_GRENS);
  prefs.putFloat("supply_min", SUPPLY_MIN);
  prefs.putUChar("max_stand",  MAX_STAND);
  prefs.putFloat("sww_sp",     SWW_SETPOINT);
  prefs.putUChar("sww_max_st", SWW_MAX_STAND);
  prefs.putBool("kamer_water", kamer_in_water);
  prefs.putFloat("koel_db",    KOEL_DEADBAND);
  prefs.end();
  Serial.println("NVS: settings opgeslagen");
}

void eeprom_load(){
  prefs.begin("chofu", true);  // read-only
  setpoint           = prefs.getFloat("setpoint",   28.0f);
  Kp                 = prefs.getFloat("Kp",         75.0f);
  Ki                 = prefs.getFloat("Ki",          0.800f);
  Kd                 = prefs.getFloat("Kd",          0.010f);
  STOOKLIJN_GRENS    = prefs.getFloat("sl_grens",   15.0f);
  STOOKLIJN_FACTOR   = prefs.getFloat("sl_factor",   0.68f);
  T_VORST            = prefs.getFloat("t_vorst",     4.0f);
  SUPPLY_MAX         = prefs.getFloat("supply_max", 60.0f);
  KOELING_MIN_BUITEN = prefs.getFloat("koel_min_b", 18.0f);
  STOOKLIJN_UIT_GRENS= prefs.getFloat("sl_uit",     15.0f);
  ff_UA_house        = prefs.getFloat("ff_ua_huis", 272.5f);
  ff_UA_emitter      = prefs.getFloat("ff_ua_emit", 267.5f);
  uint8_t m          = prefs.getUChar("modus",       (uint8_t)Modus::AUTO);
  modus = (m <= (uint8_t)Modus::FF_WATER && m != (uint8_t)Modus::HANDMATIG)
          ? (Modus)m : Modus::AUTO;
  Kp_water           = prefs.getFloat("Kp_water",   50.0f);
  Ki_water           = prefs.getFloat("Ki_water",    0.800f);
  Kd_water           = prefs.getFloat("Kd_water",    0.010f);
  STOOKLIJN_AAN_GRENS= prefs.getFloat("sl_aan",     13.0f);
  SUPPLY_MIN         = prefs.getFloat("supply_min", 17.0f);
  MAX_STAND          = prefs.getUChar("max_stand",  8);
  if(MAX_STAND < 1 || MAX_STAND > 8) MAX_STAND = 8;
  SWW_SETPOINT       = prefs.getFloat("sww_sp",     50.0f);
  if(isnan(SWW_SETPOINT) || SWW_SETPOINT < 30 || SWW_SETPOINT > 60) SWW_SETPOINT = 50.0f;
  SWW_MAX_STAND      = prefs.getUChar("sww_max_st", 8);
  if(SWW_MAX_STAND < 1 || SWW_MAX_STAND > 8) SWW_MAX_STAND = 8;
  kamer_in_water     = prefs.getBool("kamer_water", true);
  KOEL_DEADBAND      = prefs.getFloat("koel_db",     1.0f);
  if(isnan(KOEL_DEADBAND) || KOEL_DEADBAND < 0 || KOEL_DEADBAND > 5) KOEL_DEADBAND = 1.0f;
  prefs.end();
  Serial.print("NVS: geladen - SP:"); Serial.print(setpoint,1);
  Serial.print(" PID:"); Serial.print(Kp,2); Serial.print("/"); Serial.print(Ki,3); Serial.print("/"); Serial.println(Kd,2);
  Serial.print("  FF UA huis:"); Serial.print(ff_UA_house,0);
  Serial.print(" emitter:"); Serial.println(ff_UA_emitter,0);
}

#endif // ARDUINO_UNOR4_WIFI
