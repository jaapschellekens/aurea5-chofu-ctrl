#include "mqtt.h"
#include "eeprom.h"   // voor eeprom_save()
#include "regelaar.h" // voor ctrl.reset_pid() / reset_ff() / koude_start()
#include "adam.h"     // voor adam_beschikbaar() / status

// ═══════════════════════════════════════════════════════════════
//  LOGGING
// ═══════════════════════════════════════════════════════════════

// Definitie ZONDER default-parameter (staat in mqtt.h)
void mqtt_log(String message, String level){
  Serial.println(message);
  uint32_t nu = millis();
  if(nu - laatste_log_ms < LOG_THROTTLE_MS && level != "ERROR") return;
  laatste_log_ms = nu;
  if(mqtt_logging_enabled && mqttClient.connected()){
    String topic = "chofu/log/" + level;
    mqttClient.beginMessage(topic);
    mqttClient.print(message);
    mqttClient.endMessage();
  }
}

void stuur_alert(String msg){
  Serial.println("ALERT: " + msg);
  if(mqttClient.connected()){
    mqttClient.beginMessage("chofu/log/WARNING", (unsigned long)msg.length());
    mqttClient.print(msg);
    mqttClient.endMessage();
    mqttClient.beginMessage("chofu/alert", (unsigned long)msg.length(), true);
    mqttClient.print(msg);
    mqttClient.endMessage();
  }
}

// ═══════════════════════════════════════════════════════════════
//  PROTOCOL LOGGING
// ═══════════════════════════════════════════════════════════════

// Publiceert hex-dump van telegram op chofu/proto/<subtopic>
// extra: optionele tekst achter de hex (bijv. " | CS FOUT")
void mqtt_proto(const char* subtopic, uint8_t* buf, uint8_t len, const String& extra){
  if(!proto_logging || !mqttClient.connected()) return;
  // bouw hex string: "19 01 00 01 00 ... CS"
  String hex;
  hex.reserve(len * 3 + extra.length() + 4);
  for(uint8_t i = 0; i < len; i++){
    if(i) hex += ' ';
    if(buf[i] < 0x10) hex += '0';
    hex += String(buf[i], HEX);
  }
  if(extra.length()) hex += extra;
  String topic = "chofu/proto/";
  topic += subtopic;
  mqttClient.beginMessage(topic, (unsigned long)hex.length());
  mqttClient.print(hex);
  mqttClient.endMessage();
}

// ═══════════════════════════════════════════════════════════════
//  WATCHDOG
// ═══════════════════════════════════════════════════════════════

void check_mqtt_watchdog(){
  if(vorige_mqtt_rx_ms == 0) return;
  uint32_t nu = millis();
  if(nu - vorige_mqtt_rx_ms < (uint32_t)MQTT_WATCHDOG_MS) return;
  vorige_mqtt_rx_ms = nu;
  if(modus != Modus::AUTO){
    modus = Modus::AUTO; ctrl.reset_pid(); ctrl.reset_ff();
    stuur_alert("MQTT watchdog: geen contact > " + String(MQTT_WATCHDOG_MS/60000) + " min, terug naar auto");
  } else {
    stuur_alert("MQTT watchdog: geen contact > " + String(MQTT_WATCHDOG_MS/60000) + " min");
  }
}

// ═══════════════════════════════════════════════════════════════
//  MQTT ONTVANGEN
// ═══════════════════════════════════════════════════════════════

void mqtt_ontvang(int len){
  vorige_mqtt_rx_ms = millis();
  String topic = mqttClient.messageTopic();
  String payload = "";
  while(mqttClient.available()) payload += (char)mqttClient.read();

  // Throttle Serial-output: bij hoog MQTT-volume vult de USB-CDC TX-buffer
  // en blokkeert Serial.println() de hele callback.
  static uint32_t laatste_serial_mqtt_ms = 0;
  if(millis() - laatste_serial_mqtt_ms >= 200){
    Serial.print("MQTT: "); Serial.print(topic); Serial.print("="); Serial.println(payload);
    laatste_serial_mqtt_ms = millis();
  }

  if(topic == "chofu/cmd/lcd"){
    lcd_enabled = (payload == "1");
    if(lcd_enabled) lcd.backlight(); else { lcd.noBacklight(); lcd.clear(); }
  }
  else if(topic == "chofu/cmd/power"){
    modus = Modus::HANDMATIG; handmatig_stand = (payload == "1") ? 1 : 0;
  }
  else if(topic == "chofu/cmd/stand"){
    int val = payload.toInt();
    if(val >= 0 && val <= 12){ modus = Modus::HANDMATIG; handmatig_stand = val;
      mqtt_log("Handmatig stand: " + String(val), "INFO"); }
  }
  else if(topic == "chofu/cmd/stooklijn_basis"){
    float val = payload.toFloat();
    if(val >= 20 && val <= 45){ setpoint = val; eeprom_save(); }
  }
  else if(topic == "chofu/cmd/kp"){ float v=payload.toFloat(); if(v>=0.1&&v<=500.0){ Kp=v; eeprom_save(); } }
  else if(topic == "chofu/cmd/ki"){ float v=payload.toFloat(); if(v>=0.0&&v<=5.0){   Ki=v; eeprom_save(); } }
  else if(topic == "chofu/cmd/kd"){ float v=payload.toFloat(); if(v>=0.0&&v<=50.0){  Kd=v; eeprom_save(); } }
  else if(topic == "chofu/cmd/kp_water"){ float v=payload.toFloat(); if(v>=0.1&&v<=500.0){ Kp_water=v; eeprom_save(); } }
  else if(topic == "chofu/cmd/ki_water"){ float v=payload.toFloat(); if(v>=0.0&&v<=5.0){   Ki_water=v; eeprom_save(); } }
  else if(topic == "chofu/cmd/kd_water"){ float v=payload.toFloat(); if(v>=0.0&&v<=50.0){  Kd_water=v; eeprom_save(); } }
  else if(topic == "chofu/cmd/modus"){
    if(payload == "auto" || payload == "handmatig" || payload == "water" ||
       payload == "ff_auto" || payload == "ff_water"){
      Modus nieuwe_modus = str_naar_modus(payload);
      bool modus_gewijzigd = (nieuwe_modus != modus);
      modus = nieuwe_modus;
      if(modus_gewijzigd && modus != Modus::HANDMATIG){ ctrl.koude_start(millis()); }
      // Adam-bron is alleen geldig in ff_water — forceer terug naar MQTT bij andere modus.
      if(bron == Bron::ADAM && modus != Modus::FF_WATER){
        bron = Bron::MQTT;
        mqtt_log("Bron→mqtt (adam alleen in ff_water)", "WARNING");
        mqttClient.beginMessage("chofu/bron", true); mqttClient.print("mqtt"); mqttClient.endMessage();
      }
      eeprom_save();
      mqtt_log("Modus: " + String(modus_naar_str(modus)), "INFO");
    }
  }
  else if(topic == "chofu/cmd/t_vorst"){
    float val = payload.toFloat();
    if(val >= -10 && val <= 10){ T_VORST = val; eeprom_save(); }
  }
  else if(topic == "chofu/cmd/stooklijn_grens"){
    float val = payload.toFloat();
    if(val >= 0 && val <= 25){ STOOKLIJN_GRENS = val; eeprom_save(); }
  }
  else if(topic == "chofu/cmd/stooklijn_factor"){
    float val = payload.toFloat();
    if(val >= 0.1 && val <= 5.0){ STOOKLIJN_FACTOR = val; eeprom_save(); }
  }
  else if(topic == "chofu/cmd/koeling"){
    bool gewenst = (payload == "1");
    if(gewenst && modus != Modus::FF_AUTO && modus != Modus::FF_WATER && modus != Modus::HANDMATIG){
      stuur_alert("Koeling alleen in FF_AUTO/FF_WATER/HANDMATIG — verzoek genegeerd");
    } else {
      koeling_modus = gewenst; ctrl.reset_pid();
      mqtt_log(koeling_modus ? "Koeling aan" : "Verwarming aan", "INFO");
    }
  }
  else if(topic == "chofu/cmd/supply_min"){
    float val = payload.toFloat();
    if(val >= 10 && val <= 25){ SUPPLY_MIN = val; eeprom_save(); }
  }
  else if(topic == "chofu/cmd/koeling_afschakel"){
    float val = payload.toFloat();
    if(val >= 0.1f && val <= 5.0f){ KOELING_AFSCHAKEL = val; }
  }
  else if(topic == "chofu/cmd/water_setpoint"){
    if(bron == Bron::ADAM) return;   // Adam is bron: HA mag niet overschrijven
    float val = payload.toFloat();
    if(val != 0.0f && val < WATER_SP_MIN){ return; }   // 1..(min-1)°C: ongeldig, negeer
    if(val >= 0 && val <= 55){ t_water_gewenst = val;
      mqtt_log("Water SP: " + String(t_water_gewenst,1) + "C (0=geen warmtevraag)", "INFO"); }
  }
  else if(topic == "chofu/cmd/bron"){
    if(payload == "adam"){
      if(!adam_beschikbaar()){
        mqtt_log("Bron adam genegeerd: USE_ADAM uit", "WARNING");
      } else if(modus != Modus::FF_WATER){
        mqtt_log("Bron adam alleen in ff_water — eerst modus=ff_water", "WARNING");
      } else {
        bron = Bron::ADAM; eeprom_save();
        mqtt_log("Bron: adam", "INFO");
      }
    } else if(payload == "mqtt"){
      bron = Bron::MQTT; eeprom_save();
      mqtt_log("Bron: mqtt", "INFO");
    }
    mqttClient.beginMessage("chofu/bron", true); mqttClient.print(bron_naar_str(bron)); mqttClient.endMessage();
  }
  else if(topic == "chofu/cmd/water_sp_min"){
    float val = payload.toFloat();
    if(val >= 10 && val <= 30){ WATER_SP_MIN = val; eeprom_save();
      mqtt_log("Water SP min: " + String(WATER_SP_MIN,1) + "C", "INFO"); }
  }
  else if(topic == "chofu/cmd/supply_max"){
    float val = payload.toFloat();
    if(val >= 40 && val <= 80){ SUPPLY_MAX = val; eeprom_save(); }
  }
  else if(topic == "chofu/cmd/koeling_min_buiten"){
    float val = payload.toFloat();
    if(val >= 0 && val <= 30){ KOELING_MIN_BUITEN = val; eeprom_save(); }
  }
  else if(topic == "chofu/cmd/stooklijn_uit"){
    float val = payload.toFloat();
    if(val >= 5 && val <= 30){ STOOKLIJN_UIT_GRENS = val; eeprom_save(); }
  }
  else if(topic == "chofu/cmd/stooklijn_aan"){
    float val = payload.toFloat();
    // aan-drempel moet lager zijn dan uit-drempel voor zinvolle hysteresis
    if(val >= 0 && val <= 25 && val < STOOKLIJN_UIT_GRENS){ STOOKLIJN_AAN_GRENS = val; eeprom_save(); }
  }
  else if(topic == "chofu/cmd/ff_min_off"){
    // Waarde in minuten voor gebruiksgemak (0 = uitgeschakeld, bijv. voor HIL-testen)
    float val = payload.toFloat();
    if(val >= 0 && val <= 120){ FF_MIN_OFF_MS = (long)(val * 60000L); }
  }
  else if(topic == "chofu/cmd/ff_restart_coast"){
    float val = payload.toFloat();
    if(val >= 0.0f && val <= 5.0f){ FF_RESTART_COAST = val; }
  }
  else if(topic == "chofu/cmd/auto_hyst_down"){
    // Waarde in minuten
    float val = payload.toFloat();
    if(val >= 0 && val <= 30){ AUTO_HYST_DOWN_MS = (long)(val * 60000L); }
  }
  else if(topic == "chofu/cmd/ff_afschakel"){
    // Negatieve waarde: °C boven setpoint waarbij FF terugschakelt (bijv. -0.5)
    float val = payload.toFloat();
    if(val >= -3.0f && val <= 0.0f){ FF_AFSCHAKEL_AUTO = val; }
  }
  else if(topic == "chofu/cmd/ff_lookahead"){
    // Vooruitkijktijd voor predictieve terugschakeling in minuten (0 = uit)
    float val = payload.toFloat();
    if(val >= 0 && val <= 60){ FF_LOOKAHEAD_MS = (long)(val * 60000.0f); }
  }
  else if(topic == "chofu/cmd/ff_thermal_min_off"){
    // Min. uitschakelperiode na thermische stop in minuten (0-30)
    float val = payload.toFloat();
    if(val >= 0 && val <= 30){ FF_THERMAL_MIN_OFF_MS = (long)(val * 60000.0f); }
  }
  else if(topic == "chofu/cmd/ff_ua_house"){
    float val = payload.toFloat();
    if(val >= 50 && val <= 500){ ff_UA_house = val; eeprom_save();
      mqtt_log("FF UA huis: " + String(ff_UA_house,0), "INFO"); }
  }
  else if(topic == "chofu/cmd/ff_ua_emitter"){
    float val = payload.toFloat();
    if(val >= 50 && val <= 500){ ff_UA_emitter = val; eeprom_save();
      mqtt_log("FF UA emitter: " + String(ff_UA_emitter,0), "INFO"); }
  }
  else if(topic == "chofu/cmd/ff_save"){
    eeprom_save();
    mqtt_log("FF UA opgeslagen: huis=" + String(ff_UA_house,0) + " emitter=" + String(ff_UA_emitter,0), "INFO");
  }
  else if(topic == "chofu/cmd/kamer_setpoint"){
    if(bron == Bron::ADAM) return;   // Adam is bron: HA mag niet overschrijven
    float val = payload.toFloat();
    if(val >= 14 && val <= 30){
      t_kamer_gewenst = val;
      mqttClient.beginMessage("chofu/kamer_gewenst"); mqttClient.print(t_kamer_gewenst, 1); mqttClient.endMessage();
    }
  }
  else if(topic == "chofu/cmd/kamer"){
    if(bron == Bron::ADAM) return;   // Adam is bron: HA mag niet overschrijven
    float val = payload.toFloat();
    if(val >= 5 && val <= 35) t_kamer = val;
  }
  else if(topic == "chofu/cmd/force_start"){
    ctrl.vorige_stand_wijz_ms = 0;
    Serial.println("FORCE START - hysteresis gereset");
  }
  else if(topic == "chofu/cmd/proto_log"){
    proto_logging = (payload == "1");
    mqtt_log(proto_logging ? "Protocol logging AAN" : "Protocol logging UIT", "INFO");
  }
  // Simulatie topics
  else if(topic == "chofu/cmd/sim"){
    sim_enabled = (payload == "1");
    if(!sim_enabled){
      sim_t_supply = NAN; sim_t_return = NAN; sim_t_outside = NAN;
      sim_t_water_gewenst = NAN; sim_t_kamer = NAN; sim_t_kamer_gewenst = NAN;
    }
    mqtt_log(sim_enabled ? "Simulatie ingeschakeld" : "Simulatie uitgeschakeld", "INFO");
  }
  else if(topic == "chofu/sim/supply"){
    if(payload.length() == 0 || payload == "reset") sim_t_supply = NAN;
    else { float v = payload.toFloat(); if(v >= -10 && v <= 80) sim_t_supply = v; }
  }
  else if(topic == "chofu/sim/return"){
    if(payload.length() == 0 || payload == "reset") sim_t_return = NAN;
    else { float v = payload.toFloat(); if(v >= -10 && v <= 80) sim_t_return = v; }
  }
  else if(topic == "chofu/sim/outside"){
    if(payload.length() == 0 || payload == "reset") sim_t_outside = NAN;
    else { float v = payload.toFloat(); if(v >= -30 && v <= 50) sim_t_outside = v; }
  }
  else if(topic == "chofu/sim/water_setpoint"){
    if(payload.length() == 0 || payload == "reset") sim_t_water_gewenst = NAN;
    else { float v = payload.toFloat(); if(v >= 16 && v <= 55) sim_t_water_gewenst = v; }
  }
  else if(topic == "chofu/sim/kamer"){
    if(payload.length() == 0 || payload == "reset") sim_t_kamer = NAN;
    else { float v = payload.toFloat(); if(v >= 5 && v <= 40) sim_t_kamer = v; }
  }
  else if(topic == "chofu/sim/kamer_gewenst"){
    if(payload.length() == 0 || payload == "reset") sim_t_kamer_gewenst = NAN;
    else { float v = payload.toFloat(); if(v >= 14 && v <= 30) sim_t_kamer_gewenst = v; }
  }
  else if(topic == "chofu/sim/reset"){
    sim_t_supply = NAN; sim_t_return = NAN; sim_t_outside = NAN;
    sim_t_water_gewenst = NAN; sim_t_kamer = NAN; sim_t_kamer_gewenst = NAN;
    mqtt_log("Simulatie gereset", "INFO");
  }
  else if(topic == "chofu/cmd/hyst_slow"){
    long val = payload.toInt();
    if(val >= 100 && val <= 3600000) HYST_SLOW_MS = val;
  }
  else if(topic == "chofu/cmd/hyst_fast"){
    long val = payload.toInt();
    if(val >= 100 && val <= 3600000) HYST_FAST_MS = val;
  }
  else if(topic == "chofu/cmd/hyst_down"){
    long val = payload.toInt();
    if(val >= 100 && val <= 3600000) HYST_DOWN_MS = val;
  }
  else if(topic == "chofu/cmd/pid_interval"){
    long val = payload.toInt();
    if(val >= 100 && val <= 60000) pid_interval_ms = val;
  }

  // Stuur data niet direct vanuit de callback (re-entrant mqttClient gebruik).
  // De main loop pikt de vlag op en roept stuur_data() aan op een veilig moment.
  if (!topic.startsWith("chofu/sim/")) data_sturen_gevraagd = true;
}

// ═══════════════════════════════════════════════════════════════
//  HOME ASSISTANT DISCOVERY
// ═══════════════════════════════════════════════════════════════

void disco_pub(const char* topic, String& pl){
  mqttClient.poll();
  mqttClient.beginMessage(topic, (unsigned long)pl.length(), true);
  mqttClient.print(pl);
  mqttClient.endMessage();
  delay(30);
}

void discovery_fase1(){
  Serial.println("Discovery F1");
  String dev = "\"dev\":{\"ids\":[\"chofu_hp\"],\"name\":\"Chofu WP\",\"mf\":\"Chofu\",\"mdl\":\"AEYC\",\"sw\":\"2.0\"}";
  String avty = "\"avty_t\":\"chofu/status\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\"";
  String pl;

  pl = "{\"name\":\"Chofu Aanvoer\",\"uniq_id\":\"chofu_hp_supply\",\"stat_t\":\"chofu/supply\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/chofu_hp/supply/config", pl);
  pl = "{\"name\":\"Chofu Retour\",\"uniq_id\":\"chofu_hp_return\",\"stat_t\":\"chofu/return\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/chofu_hp/return/config", pl);
  pl = "{\"name\":\"Chofu Vermogen\",\"uniq_id\":\"chofu_hp_power\",\"stat_t\":\"chofu/vermogen\",\"unit_of_meas\":\"W\",\"dev_cla\":\"power\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/chofu_hp/power/config", pl);
  pl = "{\"name\":\"Chofu Stand\",\"uniq_id\":\"chofu_hp_stage\",\"stat_t\":\"chofu/stand\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/chofu_hp/stage/config", pl);
  pl = "{\"name\":\"Chofu Buiten\",\"uniq_id\":\"chofu_hp_outside\",\"stat_t\":\"chofu/outside\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/chofu_hp/outside/config", pl);
  pl = "{\"name\":\"Chofu Power\",\"uniq_id\":\"chofu_hp_sw\",\"cmd_t\":\"chofu/cmd/power\",\"stat_t\":\"chofu/aan\",\"pl_on\":\"1\",\"pl_off\":\"0\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/switch/chofu_hp/power/config", pl);
  stuur_data();
}

void discovery_fase2(){
  Serial.println("Discovery F2");
  String dev = "\"dev\":{\"ids\":[\"chofu_hp\"],\"name\":\"Chofu WP\",\"mf\":\"Chofu\",\"mdl\":\"AEYC\",\"sw\":\"2.0\"}";
  String avty = "\"avty_t\":\"chofu/status\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\"";
  String pl;

  pl = "{\"name\":\"Chofu Delta T\",\"uniq_id\":\"chofu_hp_delta_t\",\"stat_t\":\"chofu/delta_t\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/chofu_hp/delta_t/config", pl);
  pl = "{\"name\":\"Chofu Kamer\",\"uniq_id\":\"chofu_hp_kamer\",\"cmd_t\":\"chofu/cmd/kamer\",\"stat_t\":\"chofu/kamer\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"min\":5,\"max\":35,\"step\":0.1," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/chofu_hp/kamer/config", pl);
  pl = "{\"name\":\"Chofu Kamer Gewenst\",\"uniq_id\":\"chofu_hp_kamer_gewenst\",\"cmd_t\":\"chofu/cmd/kamer_setpoint\",\"stat_t\":\"chofu/kamer_gewenst\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"min\":14,\"max\":30,\"step\":0.5," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/chofu_hp/kamer_gewenst/config", pl);
  pl = "{\"name\":\"Chofu Stooklijn Basis\",\"uniq_id\":\"chofu_hp_setpoint\",\"cmd_t\":\"chofu/cmd/stooklijn_basis\",\"stat_t\":\"chofu/stooklijn_basis\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"min\":20,\"max\":45,\"step\":0.5," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/chofu_hp/stooklijn_basis/config", pl);
  pl = "{\"name\":\"Chofu Doel Setpoint\",\"uniq_id\":\"chofu_hp_doel_setpoint\",\"stat_t\":\"chofu/doel_setpoint\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/chofu_hp/doel_setpoint/config", pl);
  pl = "{\"name\":\"Chofu Vorstgrens\",\"uniq_id\":\"chofu_hp_t_vorst\",\"cmd_t\":\"chofu/cmd/t_vorst\",\"stat_t\":\"chofu/t_vorst\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"min\":-10,\"max\":10,\"step\":0.5," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/chofu_hp/t_vorst/config", pl);
  pl = "{\"name\":\"Chofu Water SP\",\"uniq_id\":\"chofu_hp_water_setpoint\",\"cmd_t\":\"chofu/cmd/water_setpoint\",\"stat_t\":\"chofu/water_setpoint\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"min\":0,\"max\":55,\"step\":0.5," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/chofu_hp/water_setpoint/config", pl);
  pl = "{\"name\":\"Chofu Water SP Min\",\"uniq_id\":\"chofu_hp_water_sp_min\",\"cmd_t\":\"chofu/cmd/water_sp_min\",\"stat_t\":\"chofu/water_sp_min\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"min\":10,\"max\":30,\"step\":0.5," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/chofu_hp/water_sp_min/config", pl);
  pl = "{\"name\":\"Chofu Stooklijn Grens\",\"uniq_id\":\"chofu_hp_stooklijn_grens\",\"cmd_t\":\"chofu/cmd/stooklijn_grens\",\"stat_t\":\"chofu/stooklijn_grens\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"min\":0,\"max\":25,\"step\":0.5," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/chofu_hp/stooklijn_grens/config", pl);
  pl = "{\"name\":\"Chofu Stooklijn Factor\",\"uniq_id\":\"chofu_hp_stooklijn_factor\",\"cmd_t\":\"chofu/cmd/stooklijn_factor\",\"stat_t\":\"chofu/stooklijn_factor\",\"min\":0.1,\"max\":5.0,\"step\":0.1," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/chofu_hp/stooklijn_factor/config", pl);
  pl = "{\"name\":\"Chofu Modus\",\"uniq_id\":\"chofu_hp_modus\",\"stat_t\":\"chofu/modus\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/chofu_hp/modus/config", pl);
  pl = "{\"name\":\"Chofu LCD\",\"uniq_id\":\"chofu_hp_lcd\",\"cmd_t\":\"chofu/cmd/lcd\",\"stat_t\":\"chofu/lcd\",\"pl_on\":\"1\",\"pl_off\":\"0\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/switch/chofu_hp/lcd/config", pl);
  stuur_data();
}

void discovery_fase3(){
  Serial.println("Discovery F3");
  String dev = "\"dev\":{\"ids\":[\"chofu_hp\"],\"name\":\"Chofu WP\",\"mf\":\"Chofu\",\"mdl\":\"AEYC\",\"sw\":\"2.0\"}";
  String avty = "\"avty_t\":\"chofu/status\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\"";
  String pl;

  pl = "{\"name\":\"Chofu Defrost\",\"uniq_id\":\"chofu_hp_defrost\",\"stat_t\":\"chofu/defrost\",\"pl_on\":\"1\",\"pl_off\":\"0\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/binary_sensor/chofu_hp/defrost/config", pl);
  pl = "{\"name\":\"Chofu PID\",\"uniq_id\":\"chofu_hp_pid\",\"stat_t\":\"chofu/pid\",\"unit_of_meas\":\"%\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/chofu_hp/pid/config", pl);
  pl = "{\"name\":\"Chofu Pomp\",\"uniq_id\":\"chofu_hp_pomp\",\"stat_t\":\"chofu/pomp\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/chofu_hp/pomp/config", pl);
  pl = "{\"name\":\"Chofu Comp Hz\",\"uniq_id\":\"chofu_hp_comp_hz\",\"stat_t\":\"chofu/comp_hz\",\"unit_of_meas\":\"Hz\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/chofu_hp/comp_hz/config", pl);
  pl = "{\"name\":\"Chofu Stand\",\"uniq_id\":\"chofu_hp_stand_cmd\",\"cmd_t\":\"chofu/cmd/stand\",\"stat_t\":\"chofu/stand\",\"min\":0,\"max\":12,\"step\":1," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/chofu_hp/stand_cmd/config", pl);
  pl = "{\"name\":\"Chofu Koeling\",\"uniq_id\":\"chofu_hp_koeling\",\"cmd_t\":\"chofu/cmd/koeling\",\"stat_t\":\"chofu/koeling\",\"pl_on\":\"1\",\"pl_off\":\"0\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/switch/chofu_hp/koeling/config", pl);
  pl = "{\"name\":\"Chofu Koeling Min Buiten\",\"uniq_id\":\"chofu_hp_koeling_min_buiten\",\"cmd_t\":\"chofu/cmd/koeling_min_buiten\",\"stat_t\":\"chofu/koeling_min_buiten\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"min\":0,\"max\":30,\"step\":0.5," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/chofu_hp/koeling_min_buiten/config", pl);
  pl = "{\"name\":\"Chofu Koeling Aanvoer Min\",\"uniq_id\":\"chofu_hp_supply_min\",\"cmd_t\":\"chofu/cmd/supply_min\",\"stat_t\":\"chofu/supply_min\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"min\":10,\"max\":25,\"step\":0.5," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/chofu_hp/supply_min/config", pl);
  pl = "{\"name\":\"Chofu Koeling Afschakeldrempel\",\"uniq_id\":\"chofu_hp_koeling_afschakel\",\"cmd_t\":\"chofu/cmd/koeling_afschakel\",\"stat_t\":\"chofu/koeling_afschakel\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"min\":0.1,\"max\":5,\"step\":0.1," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/chofu_hp/koeling_afschakel/config", pl);

  pl = "{\"name\":\"Chofu Modus\",\"uniq_id\":\"chofu_hp_modus_sel\",\"cmd_t\":\"chofu/cmd/modus\",\"stat_t\":\"chofu/modus\",\"options\":[\"auto\",\"water\",\"ff_auto\",\"ff_water\",\"handmatig\"]," + avty + "," + dev + "}";
  disco_pub("homeassistant/select/chofu_hp/modus_sel/config", pl);

  pl = "{\"name\":\"Chofu Alert\",\"uniq_id\":\"chofu_hp_alert\",\"stat_t\":\"chofu/alert\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/chofu_hp/alert/config", pl);
  pl = "{\"name\":\"Chofu Simulatie\",\"uniq_id\":\"chofu_hp_sim\",\"stat_t\":\"chofu/sim_actief\",\"pl_on\":\"1\",\"pl_off\":\"0\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/binary_sensor/chofu_hp/sim_actief/config", pl);
  pl = "{\"name\":\"Chofu Aanvoer Max\",\"uniq_id\":\"chofu_hp_supply_max\",\"cmd_t\":\"chofu/cmd/supply_max\",\"stat_t\":\"chofu/supply_max\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"min\":40,\"max\":80,\"step\":1," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/chofu_hp/supply_max/config", pl);
  pl = "{\"name\":\"Chofu Stooklijn Uit\",\"uniq_id\":\"chofu_hp_stooklijn_uit\",\"cmd_t\":\"chofu/cmd/stooklijn_uit\",\"stat_t\":\"chofu/stooklijn_uit\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"min\":5,\"max\":30,\"step\":0.5," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/chofu_hp/stooklijn_uit/config", pl);
  pl = "{\"name\":\"Chofu Stooklijn Aan\",\"uniq_id\":\"chofu_hp_stooklijn_aan\",\"cmd_t\":\"chofu/cmd/stooklijn_aan\",\"stat_t\":\"chofu/stooklijn_aan\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"min\":0,\"max\":25,\"step\":0.5," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/chofu_hp/stooklijn_aan/config", pl);

  pl = "{\"name\":\"Chofu FF UA Huis\",\"uniq_id\":\"chofu_hp_ff_ua_house\",\"stat_t\":\"chofu/ff_ua_house\",\"unit_of_meas\":\"W/K\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/chofu_hp/ff_ua_house/config", pl);
  pl = "{\"name\":\"Chofu FF UA Emitter\",\"uniq_id\":\"chofu_hp_ff_ua_emitter\",\"stat_t\":\"chofu/ff_ua_emitter\",\"unit_of_meas\":\"W/K\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/chofu_hp/ff_ua_emitter/config", pl);
  pl = "{\"name\":\"Chofu FF UA Huis instellen\",\"uniq_id\":\"chofu_hp_ff_ua_house_cmd\",\"cmd_t\":\"chofu/cmd/ff_ua_house\",\"stat_t\":\"chofu/ff_ua_house\",\"unit_of_meas\":\"W/K\",\"min\":50,\"max\":500,\"step\":1," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/chofu_hp/ff_ua_house_cmd/config", pl);
  pl = "{\"name\":\"Chofu FF UA Emitter instellen\",\"uniq_id\":\"chofu_hp_ff_ua_emitter_cmd\",\"cmd_t\":\"chofu/cmd/ff_ua_emitter\",\"stat_t\":\"chofu/ff_ua_emitter\",\"unit_of_meas\":\"W/K\",\"min\":50,\"max\":500,\"step\":1," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/chofu_hp/ff_ua_emitter_cmd/config", pl);

  pl = "{\"name\":\"Chofu Hyst Slow ms\",\"uniq_id\":\"chofu_hp_hyst_slow\",\"cmd_t\":\"chofu/cmd/hyst_slow\",\"stat_t\":\"chofu/hyst_slow\",\"unit_of_meas\":\"ms\",\"min\":100,\"max\":3600000,\"step\":1000," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/chofu_hp/hyst_slow/config", pl);
  pl = "{\"name\":\"Chofu Hyst Fast ms\",\"uniq_id\":\"chofu_hp_hyst_fast\",\"cmd_t\":\"chofu/cmd/hyst_fast\",\"stat_t\":\"chofu/hyst_fast\",\"unit_of_meas\":\"ms\",\"min\":100,\"max\":3600000,\"step\":1000," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/chofu_hp/hyst_fast/config", pl);
  pl = "{\"name\":\"Chofu Hyst Down ms\",\"uniq_id\":\"chofu_hp_hyst_down\",\"cmd_t\":\"chofu/cmd/hyst_down\",\"stat_t\":\"chofu/hyst_down\",\"unit_of_meas\":\"ms\",\"min\":100,\"max\":3600000,\"step\":1000," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/chofu_hp/hyst_down/config", pl);
  pl = "{\"name\":\"Chofu PID Interval ms\",\"uniq_id\":\"chofu_hp_pid_interval\",\"cmd_t\":\"chofu/cmd/pid_interval\",\"stat_t\":\"chofu/pid_interval\",\"unit_of_meas\":\"ms\",\"min\":100,\"max\":60000,\"step\":100," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/chofu_hp/pid_interval/config", pl);

  pl = "{\"name\":\"Chofu PID Kp\",\"uniq_id\":\"chofu_hp_kp\",\"cmd_t\":\"chofu/cmd/kp\",\"stat_t\":\"chofu/kp\",\"min\":0.1,\"max\":500,\"step\":0.5," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/chofu_hp/kp/config", pl);
  pl = "{\"name\":\"Chofu PID Ki\",\"uniq_id\":\"chofu_hp_ki\",\"cmd_t\":\"chofu/cmd/ki\",\"stat_t\":\"chofu/ki\",\"min\":0,\"max\":5,\"step\":0.001," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/chofu_hp/ki/config", pl);
  pl = "{\"name\":\"Chofu PID Kd\",\"uniq_id\":\"chofu_hp_kd\",\"cmd_t\":\"chofu/cmd/kd\",\"stat_t\":\"chofu/kd\",\"min\":0,\"max\":50,\"step\":0.001," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/chofu_hp/kd/config", pl);

  pl = "{\"name\":\"Chofu Water PID Kp\",\"uniq_id\":\"chofu_hp_kp_water\",\"cmd_t\":\"chofu/cmd/kp_water\",\"stat_t\":\"chofu/kp_water\",\"min\":0.1,\"max\":500,\"step\":0.5," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/chofu_hp/kp_water/config", pl);
  pl = "{\"name\":\"Chofu Water PID Ki\",\"uniq_id\":\"chofu_hp_ki_water\",\"cmd_t\":\"chofu/cmd/ki_water\",\"stat_t\":\"chofu/ki_water\",\"min\":0,\"max\":5,\"step\":0.001," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/chofu_hp/ki_water/config", pl);
  pl = "{\"name\":\"Chofu Water PID Kd\",\"uniq_id\":\"chofu_hp_kd_water\",\"cmd_t\":\"chofu/cmd/kd_water\",\"stat_t\":\"chofu/kd_water\",\"min\":0,\"max\":50,\"step\":0.001," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/chofu_hp/kd_water/config", pl);

  // Protocol logging sensors
  pl = "{\"name\":\"Chofu Proto TX\",\"uniq_id\":\"chofu_hp_proto_tx\",\"stat_t\":\"chofu/proto/tx\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/chofu_hp/proto_tx/config", pl);
  pl = "{\"name\":\"Chofu Proto RX\",\"uniq_id\":\"chofu_hp_proto_rx\",\"stat_t\":\"chofu/proto/rx\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/chofu_hp/proto_rx/config", pl);
  pl = "{\"name\":\"Chofu Proto Fout\",\"uniq_id\":\"chofu_hp_proto_err\",\"stat_t\":\"chofu/proto/err\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/chofu_hp/proto_err/config", pl);
  pl = "{\"name\":\"Chofu Protocol Log\",\"uniq_id\":\"chofu_hp_proto_log\",\"cmd_t\":\"chofu/cmd/proto_log\",\"stat_t\":\"chofu/proto_log\",\"pl_on\":\"1\",\"pl_off\":\"0\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/switch/chofu_hp/proto_log/config", pl);

  stuur_data();
}

// ═══════════════════════════════════════════════════════════════
//  DATA PUBLICEREN
// ═══════════════════════════════════════════════════════════════

void stuur_data(){
  delta_t = t_supply - t_return;
  int verm = (werkelijk_vermogen_w > 0) ? (int)werkelijk_vermogen_w : VERMOGEN[ctrl.stand];

  mqttClient.beginMessage("chofu/supply");mqttClient.print(t_supply,1);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/return");mqttClient.print(t_return,1);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/vermogen");mqttClient.print(verm);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/stand");mqttClient.print(ctrl.stand);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/outside");mqttClient.print(t_outside,1);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/aan");mqttClient.print(ctrl.wp_aan?"1":"0");mqttClient.endMessage();
  mqttClient.beginMessage("chofu/delta_t");mqttClient.print(delta_t,1);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/kamer");mqttClient.print(t_kamer,1);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/kamer_gewenst");mqttClient.print(t_kamer_gewenst,1);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/stooklijn_basis");mqttClient.print(setpoint,1);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/doel_setpoint");mqttClient.print(doel_setpoint,1);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/stooklijn_grens");mqttClient.print(STOOKLIJN_GRENS,1);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/stooklijn_factor");mqttClient.print(STOOKLIJN_FACTOR,2);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/water_setpoint");mqttClient.print(t_water_gewenst,1);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/water_sp_min");mqttClient.print(WATER_SP_MIN,1);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/koeling");mqttClient.print(koeling_modus?"1":"0");mqttClient.endMessage();
  mqttClient.beginMessage("chofu/t_vorst");mqttClient.print(T_VORST,1);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/modus");mqttClient.print(modus_naar_str(modus));mqttClient.endMessage();
  mqttClient.beginMessage("chofu/bron", true);mqttClient.print(bron_naar_str(bron));mqttClient.endMessage();
  mqttClient.beginMessage("chofu/adam/status");mqttClient.print(adam_status_str());mqttClient.endMessage();
  mqttClient.beginMessage("chofu/adam/leider");mqttClient.print(adam_leider_naam());mqttClient.endMessage();
  mqttClient.beginMessage("chofu/lcd");mqttClient.print(lcd_enabled?"1":"0");mqttClient.endMessage();
  mqttClient.beginMessage("chofu/defrost");mqttClient.print(defrost?"1":"0");mqttClient.endMessage();
  mqttClient.beginMessage("chofu/pid");mqttClient.print(ctrl.pid_output,1);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/pomp");mqttClient.print(pomp_snelheid_wp);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/comp_hz");mqttClient.print(comp_hz);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/sim_actief");mqttClient.print(sim_actief()?"1":"0");mqttClient.endMessage();
  mqttClient.beginMessage("chofu/proto_log");mqttClient.print(proto_logging?"1":"0");mqttClient.endMessage();
  mqttClient.beginMessage("chofu/supply_max");mqttClient.print(SUPPLY_MAX,1);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/koeling_min_buiten");mqttClient.print(KOELING_MIN_BUITEN,1);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/supply_min");mqttClient.print(SUPPLY_MIN,1);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/koeling_afschakel");mqttClient.print(KOELING_AFSCHAKEL,2);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/stooklijn_uit");mqttClient.print(STOOKLIJN_UIT_GRENS,1);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/stooklijn_aan");mqttClient.print(STOOKLIJN_AAN_GRENS,1);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/ff_min_off");mqttClient.print(FF_MIN_OFF_MS/60000L);mqttClient.endMessage();  // in minuten
  mqttClient.beginMessage("chofu/ff_restart_coast");mqttClient.print(FF_RESTART_COAST,2);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/auto_hyst_down");mqttClient.print(AUTO_HYST_DOWN_MS/60000L);mqttClient.endMessage();  // in minuten
  mqttClient.beginMessage("chofu/ff_afschakel");mqttClient.print(FF_AFSCHAKEL_AUTO,2);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/ff_lookahead");mqttClient.print(FF_LOOKAHEAD_MS/60000.0f,2);mqttClient.endMessage();  // in minuten
  mqttClient.beginMessage("chofu/ff_thermal_min_off");mqttClient.print(FF_THERMAL_MIN_OFF_MS/60000.0f,2);mqttClient.endMessage();  // in minuten
  mqttClient.beginMessage("chofu/hyst_slow");mqttClient.print(HYST_SLOW_MS);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/hyst_fast");mqttClient.print(HYST_FAST_MS);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/hyst_down");mqttClient.print(HYST_DOWN_MS);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/pid_interval");mqttClient.print(pid_interval_ms);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/ff_ua_house");mqttClient.print(ff_UA_house,1);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/ff_ua_emitter");mqttClient.print(ff_UA_emitter,1);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/kp");mqttClient.print(Kp,3);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/ki");mqttClient.print(Ki,4);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/kd");mqttClient.print(Kd,4);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/kp_water");mqttClient.print(Kp_water,3);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/ki_water");mqttClient.print(Ki_water,4);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/kd_water");mqttClient.print(Kd_water,4);mqttClient.endMessage();

  // Ververs retained availability zodat HA entiteiten beschikbaar blijven na discovery
  mqttClient.beginMessage("chofu/status", true, 1);
  mqttClient.print("online");
  mqttClient.endMessage();

  vorige_data_ms = millis();
}
