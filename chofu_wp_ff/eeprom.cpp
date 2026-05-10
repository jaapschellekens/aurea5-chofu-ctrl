#include <EEPROM.h>   // expliciet — ESP32 core vereist dit soms direct
#include "eeprom.h"

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
  EEPROM_COMMIT();
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
  // HANDMATIG niet herstellen na herstart — veiliger om in auto te beginnen
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
  Serial.print("EEPROM: geladen - SP:");Serial.print(setpoint,1);
  Serial.print(" PID:");Serial.print(Kp,2);Serial.print("/");Serial.print(Ki,3);Serial.print("/");Serial.println(Kd,2);
  Serial.print("  FF UA huis:");Serial.print(ff_UA_house,0);
  Serial.print(" emitter:");Serial.println(ff_UA_emitter,0);
}
