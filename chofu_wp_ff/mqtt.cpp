#include "mqtt.h"
#include "eeprom.h"   // voor eeprom_save()
#include "regelaar.h" // voor ctrl.reset_pid() / reset_ff() / koude_start()

// ═══════════════════════════════════════════════════════════════
//  LOGGING
// ═══════════════════════════════════════════════════════════════

// ── Serial-log ringbuffer (batched naar chofu/seriallog) ────────
// Vangt logregels op in een begrensde ring; wordt 1×/30s als één bericht
// gepubliceerd (alleen als seriallog_enabled). Capture is goedkoop (snprintf,
// geen MQTT) zodat het ook veilig is buiten het JGC-RX-pad.
#define SERIALLOG_LINES    20
#define SERIALLOG_LINELEN  96
static char    seriallog_buf[SERIALLOG_LINES][SERIALLOG_LINELEN];
static uint8_t seriallog_head  = 0;   // volgende schrijfpositie
static uint8_t seriallog_count = 0;   // aantal regels sinds laatste flush

void seriallog_add(const String& msg){
  snprintf(seriallog_buf[seriallog_head], SERIALLOG_LINELEN,
           "[%lu] %s", (unsigned long)(millis() / 1000), msg.c_str());
  seriallog_head = (seriallog_head + 1) % SERIALLOG_LINES;
  if(seriallog_count < SERIALLOG_LINES) seriallog_count++;
}

void seriallog_flush(){
  if(!mqttClient.connected() || seriallog_count == 0) return;
  String pl;
  pl.reserve(seriallog_count * 48);
  uint8_t start = (seriallog_head + SERIALLOG_LINES - seriallog_count) % SERIALLOG_LINES;
  for(uint8_t i = 0; i < seriallog_count; i++){
    pl += seriallog_buf[(start + i) % SERIALLOG_LINES];
    pl += '\n';
  }
  seriallog_count = 0;   // wis: volgend bericht bevat alleen nieuwe regels
  mqttClient.beginMessage(MQTT_PREFIX "/seriallog", (unsigned long)pl.length());
  mqttClient.print(pl);
  mqttClient.endMessage();
}

// Definitie ZONDER default-parameter (staat in mqtt.h)
void mqtt_log(String message, String level){
  Serial.println(message);
  seriallog_add(message);
  uint32_t nu = millis();
  if(nu - laatste_log_ms < LOG_THROTTLE_MS && level != "ERROR") return;
  laatste_log_ms = nu;
  if(mqtt_logging_enabled && mqttClient.connected()){
    String topic = MQTT_PREFIX "/log/" + level;
    mqttClient.beginMessage(topic);
    mqttClient.print(message);
    mqttClient.endMessage();
  }
}

void stuur_alert(String msg){
  Serial.println("ALERT: " + msg);
  seriallog_add("ALERT: " + msg);
  if(mqttClient.connected()){
    mqttClient.beginMessage(MQTT_PREFIX "/log/WARNING", (unsigned long)msg.length());
    mqttClient.print(msg);
    mqttClient.endMessage();
    mqttClient.beginMessage(MQTT_PREFIX "/alert", (unsigned long)msg.length(), true);
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
  String topic = MQTT_PREFIX "/proto/";
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

  if(topic == MQTT_PREFIX "/cmd/lcd"){
    lcd_enabled = (payload == "1");
    if(lcd_enabled) lcd.backlight(); else { lcd.noBacklight(); lcd.clear(); }
  }
  else if(topic == MQTT_PREFIX "/cmd/power"){
    modus = Modus::HANDMATIG; handmatig_stand = (payload == "1") ? 1 : 0;
  }
  else if(topic == MQTT_PREFIX "/cmd/stand"){
    int val = payload.toInt();
    if(val >= 0 && val <= 12){ modus = Modus::HANDMATIG; handmatig_stand = val;
      mqtt_log("Handmatig stand: " + String(val), "INFO"); }
  }
  else if(topic == MQTT_PREFIX "/cmd/stooklijn_basis"){
    float val = payload.toFloat();
    if(val >= 20 && val <= 45){ setpoint = val; eeprom_save(); }
  }
  else if(topic == MQTT_PREFIX "/cmd/kp"){ float v=payload.toFloat(); if(v>=0.1&&v<=500.0){ Kp=v; eeprom_save(); } }
  else if(topic == MQTT_PREFIX "/cmd/ki"){ float v=payload.toFloat(); if(v>=0.0&&v<=5.0){   Ki=v; eeprom_save(); } }
  else if(topic == MQTT_PREFIX "/cmd/kd"){ float v=payload.toFloat(); if(v>=0.0&&v<=50.0){  Kd=v; eeprom_save(); } }
  else if(topic == MQTT_PREFIX "/cmd/kp_water"){ float v=payload.toFloat(); if(v>=0.1&&v<=500.0){ Kp_water=v; eeprom_save(); } }
  else if(topic == MQTT_PREFIX "/cmd/ki_water"){ float v=payload.toFloat(); if(v>=0.0&&v<=5.0){   Ki_water=v; eeprom_save(); } }
  else if(topic == MQTT_PREFIX "/cmd/kd_water"){ float v=payload.toFloat(); if(v>=0.0&&v<=50.0){  Kd_water=v; eeprom_save(); } }
  else if(topic == MQTT_PREFIX "/cmd/modus"){
    if(payload == "auto" || payload == "handmatig" || payload == "water" ||
       payload == "ff_auto" || payload == "ff_water"){
      Modus nieuwe_modus = str_naar_modus(payload);
      bool modus_gewijzigd = (nieuwe_modus != modus);
      modus = nieuwe_modus;
      if(modus_gewijzigd && modus != Modus::HANDMATIG){ ctrl.koude_start(millis()); }
      eeprom_save();
      mqtt_log("Modus: " + String(modus_naar_str(modus)), "INFO");
    }
  }
  else if(topic == MQTT_PREFIX "/cmd/t_vorst"){
    float val = payload.toFloat();
    if(val >= -10 && val <= 10){ T_VORST = val; eeprom_save(); }
  }
  else if(topic == MQTT_PREFIX "/cmd/stooklijn_grens"){
    float val = payload.toFloat();
    if(val >= 0 && val <= 25){ STOOKLIJN_GRENS = val; eeprom_save(); }
  }
  else if(topic == MQTT_PREFIX "/cmd/stooklijn_factor"){
    float val = payload.toFloat();
    if(val >= 0.1 && val <= 5.0){ STOOKLIJN_FACTOR = val; eeprom_save(); }
  }
  else if(topic == MQTT_PREFIX "/cmd/koeling"){
    bool gewenst = (payload == "1");
    if(gewenst && modus != Modus::FF_AUTO && modus != Modus::FF_WATER && modus != Modus::HANDMATIG){
      stuur_alert("Koeling alleen in FF_AUTO/FF_WATER/HANDMATIG — verzoek genegeerd");
    } else {
      koeling_modus = gewenst; ctrl.reset_pid();
      mqtt_log(koeling_modus ? "Koeling aan" : "Verwarming aan", "INFO");
    }
  }
  else if(topic == MQTT_PREFIX "/cmd/supply_min"){
    float val = payload.toFloat();
    if(val >= 10 && val <= 25){ SUPPLY_MIN = val; eeprom_save(); }
  }
  else if(topic == MQTT_PREFIX "/cmd/koeling_afschakel"){
    float val = payload.toFloat();
    if(val >= 0.1f && val <= 5.0f){ KOELING_AFSCHAKEL = val; }
  }
  else if(topic == MQTT_PREFIX "/cmd/water_setpoint"){
    float val = payload.toFloat();
    if(val == 0.0f){
      t_water_gewenst = 0.0f;
      mqtt_log("Water SP: 0 (geen vraag)", "INFO");
    } else if(val < SUPPLY_MIN){
      // Te laag voor zowel verwarming als koeling: clamp op SUPPLY_MIN (condensatiebescherming)
      stuur_alert("Water SP " + String(val,1) + "C < SUPPLY_MIN " + String(SUPPLY_MIN,1) + "C -> geclampt");
      t_water_gewenst = SUPPLY_MIN;
      mqtt_log("Water SP geclampt op SUPPLY_MIN: " + String(SUPPLY_MIN,1) + "C", "INFO");
    } else if(!koeling_modus && val < WATER_SP_MIN){
      // Verwarming: tussen SUPPLY_MIN en WATER_SP_MIN is ongeldig (bijv. 17-15°C = geen zinnige warmtevraag)
      return;
    } else if(val <= 55){
      t_water_gewenst = val;
      mqtt_log("Water SP: " + String(t_water_gewenst,1) + "C", "INFO");
    }
  }
  else if(topic == MQTT_PREFIX "/cmd/water_sp_min"){
    float val = payload.toFloat();
    if(val >= 10 && val <= 30){ WATER_SP_MIN = val; eeprom_save();
      mqtt_log("Water SP min: " + String(WATER_SP_MIN,1) + "C", "INFO"); }
  }
  else if(topic == MQTT_PREFIX "/cmd/supply_max"){
    float val = payload.toFloat();
    if(val >= 40 && val <= 80){ SUPPLY_MAX = val; eeprom_save(); }
  }
  else if(topic == MQTT_PREFIX "/cmd/koeling_min_buiten"){
    float val = payload.toFloat();
    if(val >= 0 && val <= 30){ KOELING_MIN_BUITEN = val; eeprom_save(); }
  }
  else if(topic == MQTT_PREFIX "/cmd/stooklijn_uit"){
    float val = payload.toFloat();
    if(val >= 5 && val <= 30){ STOOKLIJN_UIT_GRENS = val; eeprom_save(); }
  }
  else if(topic == MQTT_PREFIX "/cmd/stooklijn_aan"){
    float val = payload.toFloat();
    // aan-drempel moet lager zijn dan uit-drempel voor zinvolle hysteresis
    if(val >= 0 && val <= 25 && val < STOOKLIJN_UIT_GRENS){ STOOKLIJN_AAN_GRENS = val; eeprom_save(); }
  }
  else if(topic == MQTT_PREFIX "/cmd/ff_min_off"){
    // Waarde in minuten voor gebruiksgemak (0 = uitgeschakeld, bijv. voor HIL-testen)
    float val = payload.toFloat();
    if(val >= 0 && val <= 120){ FF_MIN_OFF_MS = (long)(val * 60000L); }
  }
  else if(topic == MQTT_PREFIX "/cmd/ff_restart_coast"){
    float val = payload.toFloat();
    if(val >= 0.0f && val <= 5.0f){ FF_RESTART_COAST = val; }
  }
  else if(topic == MQTT_PREFIX "/cmd/auto_hyst_down"){
    // Waarde in minuten
    float val = payload.toFloat();
    if(val >= 0 && val <= 30){ AUTO_HYST_DOWN_MS = (long)(val * 60000L); }
  }
  else if(topic == MQTT_PREFIX "/cmd/ff_afschakel"){
    // Negatieve waarde: °C boven setpoint waarbij FF terugschakelt (bijv. -0.5)
    float val = payload.toFloat();
    if(val >= -3.0f && val <= 0.0f){ FF_AFSCHAKEL_AUTO = val; }
  }
  else if(topic == MQTT_PREFIX "/cmd/ff_lookahead"){
    // Vooruitkijktijd voor predictieve terugschakeling in minuten (0 = uit)
    float val = payload.toFloat();
    if(val >= 0 && val <= 60){ FF_LOOKAHEAD_MS = (long)(val * 60000.0f); }
  }
  else if(topic == MQTT_PREFIX "/cmd/ff_thermal_min_off"){
    // Min. uitschakelperiode na thermische stop in minuten (0-30)
    float val = payload.toFloat();
    if(val >= 0 && val <= 30){ FF_THERMAL_MIN_OFF_MS = (long)(val * 60000.0f); }
  }
  else if(topic == MQTT_PREFIX "/cmd/ff_ua_house"){
    float val = payload.toFloat();
    if(val >= 50 && val <= 500){ ff_UA_house = val; eeprom_save();
      mqtt_log("FF UA huis: " + String(ff_UA_house,0), "INFO"); }
  }
  else if(topic == MQTT_PREFIX "/cmd/ff_ua_emitter"){
    float val = payload.toFloat();
    if(val >= 50 && val <= 500){ ff_UA_emitter = val; eeprom_save();
      mqtt_log("FF UA emitter: " + String(ff_UA_emitter,0), "INFO"); }
  }
  else if(topic == MQTT_PREFIX "/cmd/ff_save"){
    eeprom_save();
    mqtt_log("FF UA opgeslagen: huis=" + String(ff_UA_house,0) + " emitter=" + String(ff_UA_emitter,0), "INFO");
  }
  else if(topic == MQTT_PREFIX "/cmd/kamer_setpoint"){
    float val = payload.toFloat();
    if(val >= 14 && val <= 30){
      t_kamer_gewenst = val;
      mqttClient.beginMessage(MQTT_PREFIX "/kamer_gewenst"); mqttClient.print(t_kamer_gewenst, 1); mqttClient.endMessage();
    }
  }
  else if(topic == MQTT_PREFIX "/cmd/kamer"){
    float val = payload.toFloat();
    if(val >= 5 && val <= 35){ t_kamer = val; kamer_geldig = true; }
  }
  else if(topic == MQTT_PREFIX "/cmd/force_start"){
    ctrl.vorige_stand_wijz_ms = 0;
    Serial.println("FORCE START - hysteresis gereset");
  }
  else if(topic == MQTT_PREFIX "/cmd/kamer_in_water"){
    bool val = (payload == "1");
    if(val != kamer_in_water){
      kamer_in_water = val;
      eeprom_save();
      mqtt_log(kamer_in_water ? "Kamertemp IN water-modi: AAN" : "Kamertemp IN water-modi: UIT (alleen aanvoertemp)", "INFO");
    }
    mqttClient.beginMessage(MQTT_PREFIX "/kamer_in_water"); mqttClient.print(kamer_in_water ? "1" : "0"); mqttClient.endMessage();
  }
  else if(topic == MQTT_PREFIX "/cmd/proto_log"){
    proto_logging = (payload == "1");
    mqtt_log(proto_logging ? "Protocol logging AAN" : "Protocol logging UIT", "INFO");
  }
  else if(topic == MQTT_PREFIX "/cmd/seriallog"){
    seriallog_enabled = (payload == "1");
    mqtt_log(seriallog_enabled ? "Serial-log naar MQTT AAN" : "Serial-log naar MQTT UIT", "INFO");
  }
  else if(topic == MQTT_PREFIX "/cmd/max_stand"){
    int val = payload.toInt();
    if(val >= 1 && val <= 8){ MAX_STAND = (uint8_t)val; eeprom_save();
      mqtt_log("Max stand: " + String(MAX_STAND) + " (niet-handmatig)", "INFO"); }
  }
  else if(topic == MQTT_PREFIX "/cmd/sww"){
    bool aan = (payload == "1");
    if(aan != sww_actief){
      sww_actief = aan;
      if(aan) koeling_modus = false;   // SWW is verwarmen — koeling mag niet aanstaan
      if(SWW_KLEP_ACTIEF_HOOG) digitalWrite(SWW_KLEP_PIN, aan ? HIGH : LOW);
      else                     digitalWrite(SWW_KLEP_PIN, aan ? LOW  : HIGH);
      // Klepstand direct (retained) publiceren — HA kan hierop een eigen relais sturen.
      mqttClient.beginMessage(MQTT_PREFIX "/sww_klep", true); mqttClient.print(aan?"1":"0"); mqttClient.endMessage();
      ctrl.koude_start(millis());   // schone start bij wisselen SWW ↔ verwarming
      mqtt_log(aan ? "SWW AAN (tapwater laden)" : "SWW UIT", "INFO");
    }
  }
  else if(topic == MQTT_PREFIX "/cmd/sww_setpoint"){
    float v = payload.toFloat();
    if(v >= 30 && v <= 60){ SWW_SETPOINT = v; eeprom_save();
      mqtt_log("SWW setpoint: " + String(SWW_SETPOINT,1) + "C", "INFO"); }
  }
  else if(topic == MQTT_PREFIX "/cmd/sww_max_stand"){
    int v = payload.toInt();
    if(v >= 1 && v <= 8){ SWW_MAX_STAND = (uint8_t)v; eeprom_save();
      mqtt_log("SWW max stand: " + String(SWW_MAX_STAND), "INFO"); }
  }
  // Simulatie topics
  else if(topic == MQTT_PREFIX "/cmd/sim"){
    sim_enabled = (payload == "1");
    if(!sim_enabled){
      sim_t_supply = NAN; sim_t_return = NAN; sim_t_outside = NAN;
      sim_t_water_gewenst = NAN; sim_t_kamer = NAN; sim_t_kamer_gewenst = NAN;
    }
    mqtt_log(sim_enabled ? "Simulatie ingeschakeld" : "Simulatie uitgeschakeld", "INFO");
  }
  else if(topic == MQTT_PREFIX "/sim/supply"){
    if(payload.length() == 0 || payload == "reset") sim_t_supply = NAN;
    else { float v = payload.toFloat(); if(v >= -10 && v <= 80) sim_t_supply = v; }
  }
  else if(topic == MQTT_PREFIX "/sim/return"){
    if(payload.length() == 0 || payload == "reset") sim_t_return = NAN;
    else { float v = payload.toFloat(); if(v >= -10 && v <= 80) sim_t_return = v; }
  }
  else if(topic == MQTT_PREFIX "/sim/outside"){
    if(payload.length() == 0 || payload == "reset") sim_t_outside = NAN;
    else { float v = payload.toFloat(); if(v >= -30 && v <= 50) sim_t_outside = v; }
  }
  else if(topic == MQTT_PREFIX "/sim/water_setpoint"){
    if(payload.length() == 0 || payload == "reset") sim_t_water_gewenst = NAN;
    else { float v = payload.toFloat(); if(v >= 16 && v <= 55) sim_t_water_gewenst = v; }
  }
  else if(topic == MQTT_PREFIX "/sim/kamer"){
    if(payload.length() == 0 || payload == "reset") sim_t_kamer = NAN;
    else { float v = payload.toFloat(); if(v >= 5 && v <= 40) sim_t_kamer = v; }
  }
  else if(topic == MQTT_PREFIX "/sim/kamer_gewenst"){
    if(payload.length() == 0 || payload == "reset") sim_t_kamer_gewenst = NAN;
    else { float v = payload.toFloat(); if(v >= 14 && v <= 30) sim_t_kamer_gewenst = v; }
  }
  else if(topic == MQTT_PREFIX "/sim/reset"){
    sim_t_supply = NAN; sim_t_return = NAN; sim_t_outside = NAN;
    sim_t_water_gewenst = NAN; sim_t_kamer = NAN; sim_t_kamer_gewenst = NAN;
    mqtt_log("Simulatie gereset", "INFO");
  }
  else if(topic == MQTT_PREFIX "/cmd/hyst_slow"){
    long val = payload.toInt();
    if(val >= 100 && val <= 3600000) HYST_SLOW_MS = val;
  }
  else if(topic == MQTT_PREFIX "/cmd/hyst_fast"){
    long val = payload.toInt();
    if(val >= 100 && val <= 3600000) HYST_FAST_MS = val;
  }
  else if(topic == MQTT_PREFIX "/cmd/hyst_down"){
    long val = payload.toInt();
    if(val >= 100 && val <= 3600000) HYST_DOWN_MS = val;
  }
  else if(topic == MQTT_PREFIX "/cmd/pid_interval"){
    long val = payload.toInt();
    if(val >= 100 && val <= 60000) pid_interval_ms = val;
  }

  // Stuur data niet direct vanuit de callback (re-entrant mqttClient gebruik).
  // De main loop pikt de vlag op en roept stuur_data() aan op een veilig moment.
  if (!topic.startsWith(MQTT_PREFIX "/sim/")) data_sturen_gevraagd = true;
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
  String dev = "\"dev\":{\"ids\":[\"" HA_NODE "\"],\"name\":\"" HA_DEV_NAME "\",\"mf\":\"Chofu\",\"mdl\":\"AEYC\",\"sw\":\"2.0\"}";
  String avty = "\"avty_t\":\"" MQTT_PREFIX "/status\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\"";
  String pl;

  pl = "{\"name\":\"Chofu Aanvoer\",\"uniq_id\":\"" HA_NODE "_supply\",\"stat_t\":\"" MQTT_PREFIX "/supply\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/" HA_NODE "/supply/config", pl);
  pl = "{\"name\":\"Chofu Retour\",\"uniq_id\":\"" HA_NODE "_return\",\"stat_t\":\"" MQTT_PREFIX "/return\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/" HA_NODE "/return/config", pl);
  pl = "{\"name\":\"Chofu Vermogen\",\"uniq_id\":\"" HA_NODE "_power\",\"stat_t\":\"" MQTT_PREFIX "/vermogen\",\"unit_of_meas\":\"W\",\"dev_cla\":\"power\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/" HA_NODE "/power/config", pl);
  pl = "{\"name\":\"Chofu Stand\",\"uniq_id\":\"" HA_NODE "_stage\",\"stat_t\":\"" MQTT_PREFIX "/stand\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/" HA_NODE "/stage/config", pl);
  pl = "{\"name\":\"Chofu Buiten\",\"uniq_id\":\"" HA_NODE "_outside\",\"stat_t\":\"" MQTT_PREFIX "/outside\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/" HA_NODE "/outside/config", pl);
  pl = "{\"name\":\"Chofu Power\",\"uniq_id\":\"" HA_NODE "_sw\",\"cmd_t\":\"" MQTT_PREFIX "/cmd/power\",\"stat_t\":\"" MQTT_PREFIX "/aan\",\"pl_on\":\"1\",\"pl_off\":\"0\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/switch/" HA_NODE "/power/config", pl);
  stuur_data();
}

void discovery_fase2(){
  Serial.println("Discovery F2");
  String dev = "\"dev\":{\"ids\":[\"" HA_NODE "\"],\"name\":\"" HA_DEV_NAME "\",\"mf\":\"Chofu\",\"mdl\":\"AEYC\",\"sw\":\"2.0\"}";
  String avty = "\"avty_t\":\"" MQTT_PREFIX "/status\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\"";
  String pl;

  pl = "{\"name\":\"Chofu Delta T\",\"uniq_id\":\"" HA_NODE "_delta_t\",\"stat_t\":\"" MQTT_PREFIX "/delta_t\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/" HA_NODE "/delta_t/config", pl);
  pl = "{\"name\":\"Chofu Kamer\",\"uniq_id\":\"" HA_NODE "_kamer\",\"cmd_t\":\"" MQTT_PREFIX "/cmd/kamer\",\"stat_t\":\"" MQTT_PREFIX "/kamer\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"min\":5,\"max\":35,\"step\":0.1," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/" HA_NODE "/kamer/config", pl);
  pl = "{\"name\":\"Chofu Kamer Gewenst\",\"uniq_id\":\"" HA_NODE "_kamer_gewenst\",\"cmd_t\":\"" MQTT_PREFIX "/cmd/kamer_setpoint\",\"stat_t\":\"" MQTT_PREFIX "/kamer_gewenst\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"min\":14,\"max\":30,\"step\":0.5," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/" HA_NODE "/kamer_gewenst/config", pl);
  pl = "{\"name\":\"Chofu Kamertemp in water-modi\",\"uniq_id\":\"" HA_NODE "_kamer_in_water\",\"cmd_t\":\"" MQTT_PREFIX "/cmd/kamer_in_water\",\"stat_t\":\"" MQTT_PREFIX "/kamer_in_water\",\"pl_on\":\"1\",\"pl_off\":\"0\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/switch/" HA_NODE "/kamer_in_water/config", pl);
  pl = "{\"name\":\"Chofu Stooklijn Basis\",\"uniq_id\":\"" HA_NODE "_setpoint\",\"cmd_t\":\"" MQTT_PREFIX "/cmd/stooklijn_basis\",\"stat_t\":\"" MQTT_PREFIX "/stooklijn_basis\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"min\":20,\"max\":45,\"step\":0.5," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/" HA_NODE "/stooklijn_basis/config", pl);
  pl = "{\"name\":\"Chofu Doel Setpoint\",\"uniq_id\":\"" HA_NODE "_doel_setpoint\",\"stat_t\":\"" MQTT_PREFIX "/doel_setpoint\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/" HA_NODE "/doel_setpoint/config", pl);
  pl = "{\"name\":\"Chofu Vorstgrens\",\"uniq_id\":\"" HA_NODE "_t_vorst\",\"cmd_t\":\"" MQTT_PREFIX "/cmd/t_vorst\",\"stat_t\":\"" MQTT_PREFIX "/t_vorst\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"min\":-10,\"max\":10,\"step\":0.5," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/" HA_NODE "/t_vorst/config", pl);
  pl = "{\"name\":\"Chofu Water SP\",\"uniq_id\":\"" HA_NODE "_water_setpoint\",\"cmd_t\":\"" MQTT_PREFIX "/cmd/water_setpoint\",\"stat_t\":\"" MQTT_PREFIX "/water_setpoint\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"min\":0,\"max\":55,\"step\":0.5," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/" HA_NODE "/water_setpoint/config", pl);
  pl = "{\"name\":\"Chofu Water SP Min\",\"uniq_id\":\"" HA_NODE "_water_sp_min\",\"cmd_t\":\"" MQTT_PREFIX "/cmd/water_sp_min\",\"stat_t\":\"" MQTT_PREFIX "/water_sp_min\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"min\":10,\"max\":30,\"step\":0.5," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/" HA_NODE "/water_sp_min/config", pl);
  pl = "{\"name\":\"Chofu Stooklijn Grens\",\"uniq_id\":\"" HA_NODE "_stooklijn_grens\",\"cmd_t\":\"" MQTT_PREFIX "/cmd/stooklijn_grens\",\"stat_t\":\"" MQTT_PREFIX "/stooklijn_grens\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"min\":0,\"max\":25,\"step\":0.5," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/" HA_NODE "/stooklijn_grens/config", pl);
  pl = "{\"name\":\"Chofu Stooklijn Factor\",\"uniq_id\":\"" HA_NODE "_stooklijn_factor\",\"cmd_t\":\"" MQTT_PREFIX "/cmd/stooklijn_factor\",\"stat_t\":\"" MQTT_PREFIX "/stooklijn_factor\",\"min\":0.1,\"max\":5.0,\"step\":0.1," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/" HA_NODE "/stooklijn_factor/config", pl);
  pl = "{\"name\":\"Chofu Modus\",\"uniq_id\":\"" HA_NODE "_modus\",\"stat_t\":\"" MQTT_PREFIX "/modus\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/" HA_NODE "/modus/config", pl);
  pl = "{\"name\":\"Chofu LCD\",\"uniq_id\":\"" HA_NODE "_lcd\",\"cmd_t\":\"" MQTT_PREFIX "/cmd/lcd\",\"stat_t\":\"" MQTT_PREFIX "/lcd\",\"pl_on\":\"1\",\"pl_off\":\"0\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/switch/" HA_NODE "/lcd/config", pl);
  stuur_data();
}

void discovery_fase3(){
  Serial.println("Discovery F3");
  String dev = "\"dev\":{\"ids\":[\"" HA_NODE "\"],\"name\":\"" HA_DEV_NAME "\",\"mf\":\"Chofu\",\"mdl\":\"AEYC\",\"sw\":\"2.0\"}";
  String avty = "\"avty_t\":\"" MQTT_PREFIX "/status\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\"";
  String pl;

  pl = "{\"name\":\"Chofu Defrost\",\"uniq_id\":\"" HA_NODE "_defrost\",\"stat_t\":\"" MQTT_PREFIX "/defrost\",\"pl_on\":\"1\",\"pl_off\":\"0\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/binary_sensor/" HA_NODE "/defrost/config", pl);
  pl = "{\"name\":\"Chofu PID\",\"uniq_id\":\"" HA_NODE "_pid\",\"stat_t\":\"" MQTT_PREFIX "/pid\",\"unit_of_meas\":\"%\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/" HA_NODE "/pid/config", pl);
  pl = "{\"name\":\"Chofu Pomp\",\"uniq_id\":\"" HA_NODE "_pomp\",\"stat_t\":\"" MQTT_PREFIX "/pomp\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/" HA_NODE "/pomp/config", pl);
  pl = "{\"name\":\"Chofu Comp Hz\",\"uniq_id\":\"" HA_NODE "_comp_hz\",\"stat_t\":\"" MQTT_PREFIX "/comp_hz\",\"unit_of_meas\":\"Hz\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/" HA_NODE "/comp_hz/config", pl);
  pl = "{\"name\":\"Chofu Stand\",\"uniq_id\":\"" HA_NODE "_stand_cmd\",\"cmd_t\":\"" MQTT_PREFIX "/cmd/stand\",\"stat_t\":\"" MQTT_PREFIX "/stand\",\"min\":0,\"max\":12,\"step\":1," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/" HA_NODE "/stand_cmd/config", pl);
  pl = "{\"name\":\"Chofu Koeling\",\"uniq_id\":\"" HA_NODE "_koeling\",\"cmd_t\":\"" MQTT_PREFIX "/cmd/koeling\",\"stat_t\":\"" MQTT_PREFIX "/koeling\",\"pl_on\":\"1\",\"pl_off\":\"0\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/switch/" HA_NODE "/koeling/config", pl);
  pl = "{\"name\":\"Chofu Koeling Min Buiten\",\"uniq_id\":\"" HA_NODE "_koeling_min_buiten\",\"cmd_t\":\"" MQTT_PREFIX "/cmd/koeling_min_buiten\",\"stat_t\":\"" MQTT_PREFIX "/koeling_min_buiten\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"min\":0,\"max\":30,\"step\":0.5," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/" HA_NODE "/koeling_min_buiten/config", pl);
  pl = "{\"name\":\"Chofu Koeling Aanvoer Min\",\"uniq_id\":\"" HA_NODE "_supply_min\",\"cmd_t\":\"" MQTT_PREFIX "/cmd/supply_min\",\"stat_t\":\"" MQTT_PREFIX "/supply_min\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"min\":10,\"max\":25,\"step\":0.5," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/" HA_NODE "/supply_min/config", pl);
  pl = "{\"name\":\"Chofu Koeling Afschakeldrempel\",\"uniq_id\":\"" HA_NODE "_koeling_afschakel\",\"cmd_t\":\"" MQTT_PREFIX "/cmd/koeling_afschakel\",\"stat_t\":\"" MQTT_PREFIX "/koeling_afschakel\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"min\":0.1,\"max\":5,\"step\":0.1," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/" HA_NODE "/koeling_afschakel/config", pl);

  pl = "{\"name\":\"Chofu Modus\",\"uniq_id\":\"" HA_NODE "_modus_sel\",\"cmd_t\":\"" MQTT_PREFIX "/cmd/modus\",\"stat_t\":\"" MQTT_PREFIX "/modus\",\"options\":[\"auto\",\"water\",\"ff_auto\",\"ff_water\",\"handmatig\"]," + avty + "," + dev + "}";
  disco_pub("homeassistant/select/" HA_NODE "/modus_sel/config", pl);

  pl = "{\"name\":\"Chofu Alert\",\"uniq_id\":\"" HA_NODE "_alert\",\"stat_t\":\"" MQTT_PREFIX "/alert\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/" HA_NODE "/alert/config", pl);
  pl = "{\"name\":\"Chofu Simulatie\",\"uniq_id\":\"" HA_NODE "_sim\",\"stat_t\":\"" MQTT_PREFIX "/sim_actief\",\"pl_on\":\"1\",\"pl_off\":\"0\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/binary_sensor/" HA_NODE "/sim_actief/config", pl);
  pl = "{\"name\":\"Chofu Aanvoer Max\",\"uniq_id\":\"" HA_NODE "_supply_max\",\"cmd_t\":\"" MQTT_PREFIX "/cmd/supply_max\",\"stat_t\":\"" MQTT_PREFIX "/supply_max\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"min\":40,\"max\":80,\"step\":1," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/" HA_NODE "/supply_max/config", pl);
  pl = "{\"name\":\"Chofu Stooklijn Uit\",\"uniq_id\":\"" HA_NODE "_stooklijn_uit\",\"cmd_t\":\"" MQTT_PREFIX "/cmd/stooklijn_uit\",\"stat_t\":\"" MQTT_PREFIX "/stooklijn_uit\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"min\":5,\"max\":30,\"step\":0.5," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/" HA_NODE "/stooklijn_uit/config", pl);
  pl = "{\"name\":\"Chofu Stooklijn Aan\",\"uniq_id\":\"" HA_NODE "_stooklijn_aan\",\"cmd_t\":\"" MQTT_PREFIX "/cmd/stooklijn_aan\",\"stat_t\":\"" MQTT_PREFIX "/stooklijn_aan\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"min\":0,\"max\":25,\"step\":0.5," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/" HA_NODE "/stooklijn_aan/config", pl);

  pl = "{\"name\":\"Chofu FF UA Huis\",\"uniq_id\":\"" HA_NODE "_ff_ua_house\",\"stat_t\":\"" MQTT_PREFIX "/ff_ua_house\",\"unit_of_meas\":\"W/K\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/" HA_NODE "/ff_ua_house/config", pl);
  pl = "{\"name\":\"Chofu FF UA Emitter\",\"uniq_id\":\"" HA_NODE "_ff_ua_emitter\",\"stat_t\":\"" MQTT_PREFIX "/ff_ua_emitter\",\"unit_of_meas\":\"W/K\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/" HA_NODE "/ff_ua_emitter/config", pl);
  pl = "{\"name\":\"Chofu FF UA Huis instellen\",\"uniq_id\":\"" HA_NODE "_ff_ua_house_cmd\",\"cmd_t\":\"" MQTT_PREFIX "/cmd/ff_ua_house\",\"stat_t\":\"" MQTT_PREFIX "/ff_ua_house\",\"unit_of_meas\":\"W/K\",\"min\":50,\"max\":500,\"step\":1," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/" HA_NODE "/ff_ua_house_cmd/config", pl);
  pl = "{\"name\":\"Chofu FF UA Emitter instellen\",\"uniq_id\":\"" HA_NODE "_ff_ua_emitter_cmd\",\"cmd_t\":\"" MQTT_PREFIX "/cmd/ff_ua_emitter\",\"stat_t\":\"" MQTT_PREFIX "/ff_ua_emitter\",\"unit_of_meas\":\"W/K\",\"min\":50,\"max\":500,\"step\":1," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/" HA_NODE "/ff_ua_emitter_cmd/config", pl);

  pl = "{\"name\":\"Chofu Hyst Slow ms\",\"uniq_id\":\"" HA_NODE "_hyst_slow\",\"cmd_t\":\"" MQTT_PREFIX "/cmd/hyst_slow\",\"stat_t\":\"" MQTT_PREFIX "/hyst_slow\",\"unit_of_meas\":\"ms\",\"min\":100,\"max\":3600000,\"step\":1000," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/" HA_NODE "/hyst_slow/config", pl);
  pl = "{\"name\":\"Chofu Hyst Fast ms\",\"uniq_id\":\"" HA_NODE "_hyst_fast\",\"cmd_t\":\"" MQTT_PREFIX "/cmd/hyst_fast\",\"stat_t\":\"" MQTT_PREFIX "/hyst_fast\",\"unit_of_meas\":\"ms\",\"min\":100,\"max\":3600000,\"step\":1000," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/" HA_NODE "/hyst_fast/config", pl);
  pl = "{\"name\":\"Chofu Hyst Down ms\",\"uniq_id\":\"" HA_NODE "_hyst_down\",\"cmd_t\":\"" MQTT_PREFIX "/cmd/hyst_down\",\"stat_t\":\"" MQTT_PREFIX "/hyst_down\",\"unit_of_meas\":\"ms\",\"min\":100,\"max\":3600000,\"step\":1000," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/" HA_NODE "/hyst_down/config", pl);
  pl = "{\"name\":\"Chofu PID Interval ms\",\"uniq_id\":\"" HA_NODE "_pid_interval\",\"cmd_t\":\"" MQTT_PREFIX "/cmd/pid_interval\",\"stat_t\":\"" MQTT_PREFIX "/pid_interval\",\"unit_of_meas\":\"ms\",\"min\":100,\"max\":60000,\"step\":100," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/" HA_NODE "/pid_interval/config", pl);

  pl = "{\"name\":\"Chofu PID Kp\",\"uniq_id\":\"" HA_NODE "_kp\",\"cmd_t\":\"" MQTT_PREFIX "/cmd/kp\",\"stat_t\":\"" MQTT_PREFIX "/kp\",\"min\":0.1,\"max\":500,\"step\":0.5," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/" HA_NODE "/kp/config", pl);
  pl = "{\"name\":\"Chofu PID Ki\",\"uniq_id\":\"" HA_NODE "_ki\",\"cmd_t\":\"" MQTT_PREFIX "/cmd/ki\",\"stat_t\":\"" MQTT_PREFIX "/ki\",\"min\":0,\"max\":5,\"step\":0.001," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/" HA_NODE "/ki/config", pl);
  pl = "{\"name\":\"Chofu PID Kd\",\"uniq_id\":\"" HA_NODE "_kd\",\"cmd_t\":\"" MQTT_PREFIX "/cmd/kd\",\"stat_t\":\"" MQTT_PREFIX "/kd\",\"min\":0,\"max\":50,\"step\":0.001," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/" HA_NODE "/kd/config", pl);

  pl = "{\"name\":\"Chofu Water PID Kp\",\"uniq_id\":\"" HA_NODE "_kp_water\",\"cmd_t\":\"" MQTT_PREFIX "/cmd/kp_water\",\"stat_t\":\"" MQTT_PREFIX "/kp_water\",\"min\":0.1,\"max\":500,\"step\":0.5," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/" HA_NODE "/kp_water/config", pl);
  pl = "{\"name\":\"Chofu Water PID Ki\",\"uniq_id\":\"" HA_NODE "_ki_water\",\"cmd_t\":\"" MQTT_PREFIX "/cmd/ki_water\",\"stat_t\":\"" MQTT_PREFIX "/ki_water\",\"min\":0,\"max\":5,\"step\":0.001," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/" HA_NODE "/ki_water/config", pl);
  pl = "{\"name\":\"Chofu Water PID Kd\",\"uniq_id\":\"" HA_NODE "_kd_water\",\"cmd_t\":\"" MQTT_PREFIX "/cmd/kd_water\",\"stat_t\":\"" MQTT_PREFIX "/kd_water\",\"min\":0,\"max\":50,\"step\":0.001," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/" HA_NODE "/kd_water/config", pl);

  // Protocol logging sensors
  pl = "{\"name\":\"Chofu Proto TX\",\"uniq_id\":\"" HA_NODE "_proto_tx\",\"stat_t\":\"" MQTT_PREFIX "/proto/tx\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/" HA_NODE "/proto_tx/config", pl);
  pl = "{\"name\":\"Chofu Proto RX\",\"uniq_id\":\"" HA_NODE "_proto_rx\",\"stat_t\":\"" MQTT_PREFIX "/proto/rx\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/" HA_NODE "/proto_rx/config", pl);
  pl = "{\"name\":\"Chofu Proto Fout\",\"uniq_id\":\"" HA_NODE "_proto_err\",\"stat_t\":\"" MQTT_PREFIX "/proto/err\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/" HA_NODE "/proto_err/config", pl);
  pl = "{\"name\":\"Chofu Protocol Log\",\"uniq_id\":\"" HA_NODE "_proto_log\",\"cmd_t\":\"" MQTT_PREFIX "/cmd/proto_log\",\"stat_t\":\"" MQTT_PREFIX "/proto_log\",\"pl_on\":\"1\",\"pl_off\":\"0\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/switch/" HA_NODE "/proto_log/config", pl);
  pl = "{\"name\":\"Chofu Serial Log\",\"uniq_id\":\"" HA_NODE "_seriallog\",\"cmd_t\":\"" MQTT_PREFIX "/cmd/seriallog\",\"stat_t\":\"" MQTT_PREFIX "/seriallog_state\",\"pl_on\":\"1\",\"pl_off\":\"0\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/switch/" HA_NODE "/seriallog/config", pl);
  pl = "{\"name\":\"Chofu Serial Log Feed\",\"uniq_id\":\"" HA_NODE "_seriallog_feed\",\"stat_t\":\"" MQTT_PREFIX "/seriallog\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/" HA_NODE "/seriallog_feed/config", pl);
  pl = "{\"name\":\"Chofu Max Stand\",\"uniq_id\":\"" HA_NODE "_max_stand\",\"cmd_t\":\"" MQTT_PREFIX "/cmd/max_stand\",\"stat_t\":\"" MQTT_PREFIX "/max_stand\",\"min\":1,\"max\":8,\"step\":1," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/" HA_NODE "/max_stand/config", pl);
  pl = "{\"name\":\"Chofu SWW\",\"uniq_id\":\"" HA_NODE "_sww\",\"cmd_t\":\"" MQTT_PREFIX "/cmd/sww\",\"stat_t\":\"" MQTT_PREFIX "/sww\",\"pl_on\":\"1\",\"pl_off\":\"0\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/switch/" HA_NODE "/sww/config", pl);
  pl = "{\"name\":\"Chofu SWW Setpoint\",\"uniq_id\":\"" HA_NODE "_sww_setpoint\",\"cmd_t\":\"" MQTT_PREFIX "/cmd/sww_setpoint\",\"stat_t\":\"" MQTT_PREFIX "/sww_setpoint\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"min\":30,\"max\":60,\"step\":0.5," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/" HA_NODE "/sww_setpoint/config", pl);
  pl = "{\"name\":\"Chofu SWW Max Stand\",\"uniq_id\":\"" HA_NODE "_sww_max_stand\",\"cmd_t\":\"" MQTT_PREFIX "/cmd/sww_max_stand\",\"stat_t\":\"" MQTT_PREFIX "/sww_max_stand\",\"min\":1,\"max\":8,\"step\":1," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/" HA_NODE "/sww_max_stand/config", pl);
  pl = "{\"name\":\"Chofu SWW Klep\",\"uniq_id\":\"" HA_NODE "_sww_klep\",\"stat_t\":\"" MQTT_PREFIX "/sww_klep\",\"pl_on\":\"1\",\"pl_off\":\"0\",\"dev_cla\":\"opening\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/binary_sensor/" HA_NODE "/sww_klep/config", pl);

  stuur_data();
}

// ═══════════════════════════════════════════════════════════════
//  DATA PUBLICEREN
// ═══════════════════════════════════════════════════════════════

void stuur_data(){
  delta_t = t_supply - t_return;
  int verm = (werkelijk_vermogen_w > 0) ? (int)werkelijk_vermogen_w : VERMOGEN[ctrl.stand];

  mqttClient.beginMessage(MQTT_PREFIX "/supply");mqttClient.print(t_supply,1);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/return");mqttClient.print(t_return,1);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/vermogen");mqttClient.print(verm);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/stand");mqttClient.print(ctrl.stand);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/outside");mqttClient.print(t_outside,1);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/aan");mqttClient.print(ctrl.wp_aan?"1":"0");mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/delta_t");mqttClient.print(delta_t,1);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/kamer");mqttClient.print(t_kamer,1);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/kamer_gewenst");mqttClient.print(t_kamer_gewenst,1);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/kamer_geldig");mqttClient.print(kamer_geldig?"1":"0");mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/kamer_in_water");mqttClient.print(kamer_in_water?"1":"0");mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/stooklijn_basis");mqttClient.print(setpoint,1);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/doel_setpoint");mqttClient.print(doel_setpoint,1);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/stooklijn_grens");mqttClient.print(STOOKLIJN_GRENS,1);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/stooklijn_factor");mqttClient.print(STOOKLIJN_FACTOR,2);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/water_setpoint");mqttClient.print(t_water_gewenst,1);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/water_sp_min");mqttClient.print(WATER_SP_MIN,1);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/koeling");mqttClient.print(koeling_modus?"1":"0");mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/t_vorst");mqttClient.print(T_VORST,1);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/modus");mqttClient.print(modus_naar_str(modus));mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/lcd");mqttClient.print(lcd_enabled?"1":"0");mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/defrost");mqttClient.print(defrost?"1":"0");mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/pid");mqttClient.print(ctrl.pid_output,1);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/pomp");mqttClient.print(pomp_snelheid_wp);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/comp_hz");mqttClient.print(comp_hz);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/sim_actief");mqttClient.print(sim_actief()?"1":"0");mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/proto_log");mqttClient.print(proto_logging?"1":"0");mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/seriallog_state");mqttClient.print(seriallog_enabled?"1":"0");mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/supply_max");mqttClient.print(SUPPLY_MAX,1);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/max_stand");mqttClient.print(MAX_STAND);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/sww");mqttClient.print(sww_actief?"1":"0");mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/sww_klep", true);mqttClient.print(sww_actief?"1":"0");mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/sww_setpoint");mqttClient.print(SWW_SETPOINT,1);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/sww_max_stand");mqttClient.print(SWW_MAX_STAND);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/koeling_min_buiten");mqttClient.print(KOELING_MIN_BUITEN,1);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/supply_min");mqttClient.print(SUPPLY_MIN,1);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/koeling_afschakel");mqttClient.print(KOELING_AFSCHAKEL,2);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/stooklijn_uit");mqttClient.print(STOOKLIJN_UIT_GRENS,1);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/stooklijn_aan");mqttClient.print(STOOKLIJN_AAN_GRENS,1);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/ff_min_off");mqttClient.print(FF_MIN_OFF_MS/60000L);mqttClient.endMessage();  // in minuten
  mqttClient.beginMessage(MQTT_PREFIX "/ff_restart_coast");mqttClient.print(FF_RESTART_COAST,2);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/auto_hyst_down");mqttClient.print(AUTO_HYST_DOWN_MS/60000L);mqttClient.endMessage();  // in minuten
  mqttClient.beginMessage(MQTT_PREFIX "/ff_afschakel");mqttClient.print(FF_AFSCHAKEL_AUTO,2);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/ff_lookahead");mqttClient.print(FF_LOOKAHEAD_MS/60000.0f,2);mqttClient.endMessage();  // in minuten
  mqttClient.beginMessage(MQTT_PREFIX "/ff_thermal_min_off");mqttClient.print(FF_THERMAL_MIN_OFF_MS/60000.0f,2);mqttClient.endMessage();  // in minuten
  mqttClient.beginMessage(MQTT_PREFIX "/hyst_slow");mqttClient.print(HYST_SLOW_MS);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/hyst_fast");mqttClient.print(HYST_FAST_MS);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/hyst_down");mqttClient.print(HYST_DOWN_MS);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/pid_interval");mqttClient.print(pid_interval_ms);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/ff_ua_house");mqttClient.print(ff_UA_house,1);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/ff_ua_emitter");mqttClient.print(ff_UA_emitter,1);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/kp");mqttClient.print(Kp,3);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/ki");mqttClient.print(Ki,4);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/kd");mqttClient.print(Kd,4);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/kp_water");mqttClient.print(Kp_water,3);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/ki_water");mqttClient.print(Ki_water,4);mqttClient.endMessage();
  mqttClient.beginMessage(MQTT_PREFIX "/kd_water");mqttClient.print(Kd_water,4);mqttClient.endMessage();

  // Ververs retained availability zodat HA entiteiten beschikbaar blijven na discovery
  mqttClient.beginMessage(MQTT_PREFIX "/status", true, 1);
  mqttClient.print("online");
  mqttClient.endMessage();

  vorige_data_ms = millis();
}
