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
  Serial.print("EEPROM: geladen - SP:");Serial.print(setpoint,1);
  Serial.print(" PID:");Serial.print(Kp,2);Serial.print("/");Serial.print(Ki,3);Serial.print("/");Serial.println(Kd,2);
  Serial.print("  FF UA huis:");Serial.print(ff_UA_house,0);
  Serial.print(" emitter:");Serial.println(ff_UA_emitter,0);
}
