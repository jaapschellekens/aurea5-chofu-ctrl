

/*
 * ╔═══════════════════════════════════════════════════════════════╗
 * ║  Kromhout WP Controller v2.0 — FF modus                     ║
 * ╚═══════════════════════════════════════════════════════════════╝
 *
 * Arduino UNO R4 WiFi
 * Chofu AEYC-0643XU-CH Warmtepomp + Atlantic Aurea Controlbox
 *
 * Nieuw t.o.v. v1.0:
 *   "ff_auto"  — Feedforward op kamertemperatuur
 *                Onderhoudstand = UA_house_est × (T_set − T_buiten) / COP
 *                Leert UA_house online uit P_hp / (T_kamer − T_buiten)
 *
 *   "ff_water" — Feedforward op aanvoertemperatuur (eigen stooklijn)
 *                Onderhoudstand via P_house / COP_op_T_supply_nodig
 *                Leert UA_emitter online uit P_hp / (T_aanvoer − T_kamer)
 *                Geen oscillatie door breed doodsband (−1.5°C) + hyst 10 min
 *
 *   Leerwaarden (ff_UA_house, ff_UA_emitter) worden in EEPROM opgeslagen
 *   zodat de gecalibreerde waarden een herstart overleven.
 *
 * Bestaande modi blijven ongewijzigd:
 *   "auto"      — Kamer PID (t_supply → stooklijn setpoint)
 *   "water"     — Aanvoer PID (t_supply → t_water_gewenst)
 *   "handmatig" — Vaste stand
 */

#if defined(ARDUINO_UNOR4_WIFI)
  #include <WiFiS3.h>
  #include <Arduino_LED_Matrix.h>
#else
  #include <WiFi.h>      // ESP32 / andere boards
#endif
#include <ArduinoMqttClient.h>
#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>

// ═══════════════════════════════════════════════════════════════
//  CONFIGURATIE - PAS HIER AAN
// ═══════════════════════════════════════════════════════════════

// WiFi & MQTT — credentials staan in config.h (niet in git)
#include "config.h"

// ═══════════════════════════════════════════════════════════════
//  MODUS TYPE
// ═══════════════════════════════════════════════════════════════

enum class Modus { AUTO, FF_AUTO, WATER, FF_WATER, HANDMATIG };

static const char* modus_naar_str(Modus m){
  switch(m){
    case Modus::FF_AUTO:   return "ff_auto";
    case Modus::WATER:     return "water";
    case Modus::FF_WATER:  return "ff_water";
    case Modus::HANDMATIG: return "handmatig";
    default:               return "auto";
  }
}

static Modus str_naar_modus(const String& s){
  if(s == "ff_auto")   return Modus::FF_AUTO;
  if(s == "water")     return Modus::WATER;
  if(s == "ff_water")  return Modus::FF_WATER;
  if(s == "handmatig") return Modus::HANDMATIG;
  return Modus::AUTO;
}

// Hardware pins — Chofu serieel op Serial1 (D0=RX, D1=TX)
#define USE_LCD        true
#define USE_LED_MATRIX true   // alleen effectief op UNO R4 WiFi

// ── Board-specifieke instellingen ────────────────────────────────
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

// PID Parameters (instelbaar via MQTT)
float Kp = 19.9;
float Ki = 0.084;
float Kd = 0.036;

// Hysteresis tijden
long HYST_SLOW_MS = 600000;  // 10 minuten
long HYST_FAST_MS = 120000;  //  2 minuten
long HYST_DOWN_MS = 300000;  //  5 minuten
long pid_interval_ms = 5000; //  5 seconden

// Stooklijn parameters
float STOOKLIJN_GRENS  = 15.0;
float STOOKLIJN_FACTOR = 0.68;
float T_VORST          = 4.0;

// Safeguards
float SUPPLY_MAX           = 60.0;
float KOELING_MIN_BUITEN   = 18.0;
float STOOKLIJN_UIT_GRENS  = 15.0;
long  MQTT_WATCHDOG_MS     = 7200000;

// FF parameters (instelbaar via MQTT, opgeslagen in EEPROM)
float ff_UA_house   = 272.5;  // [W/K] lerende UA huis (auto) — geoptimaliseerd KGE
float ff_UA_emitter = 267.5;  // [W/K] lerende UA emitter (water) — UA_eff uit kalibratie
const float FF_LEARN_RATE  = 0.0002;  // tijdconstante ~7 uur (was 42 min)
const float FF_KI_AUTO     = 0.026f;  // integraalversterking auto  — geoptimaliseerd KGE
const float FF_KI_WATER    = 0.017f;  // integraalversterking water — geoptimaliseerd KGE
const float FF_COAST_AUTO  = 0.54f;   // anticipatiezone auto  [°C] — geoptimaliseerd KGE
const float FF_COAST_WATER = 4.76f;   // anticipatiezone water [°C] — geoptimaliseerd KGE

// Vermogen per stand (W)
const int VERMOGEN[] = {0, 240, 420, 640, 850, 1050, 1250, 1450, 1550, 1650, 1700, 1750, 1800};

// ═══════════════════════════════════════════════════════════════
//  GLOBALE VARIABELEN
// ═══════════════════════════════════════════════════════════════

HardwareSerial& chofuSerial = Serial1;
WiFiClient wifiClient;
MqttClient mqttClient(wifiClient);
LiquidCrystal_I2C lcd(0x27, 16, 2);
#if defined(ARDUINO_UNOR4_WIFI)
ArduinoLEDMatrix  matrix;
#endif
WiFiServer webServer(80);

// Warmtepomp data
float t_supply = 25.0, t_return = 20.0, t_outside = 5.0;
uint8_t comp_hz = 0;
uint8_t pomp_snelheid_wp = 0;
bool defrost = false;
float werkelijk_vermogen_w = 0;

// Kamer
float t_kamer = 19.0;
float t_kamer_gewenst = 19.0;

// Water
float t_water_gewenst = 32.0;
bool koeling_modus = false;

// Regelparameters
float setpoint = 28.0;
float doel_setpoint = 40.0;
float delta_t = 5.0;
bool lcd_enabled = true;
Modus modus = Modus::AUTO;
uint8_t handmatig_stand = 1;

// Controller toestand — gegroepeerd zodat resets altijd compleet zijn
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
  // Alleen PID integralen wissen (bijv. bij modus-wissel zonder afsluiten)
  void reset_pid() { pid_integraal = 0; pid_vorige_fout = 0; }
  void reset_ff()  { ff_integraal = 0; }
};
ControllerState ctrl;

// Protocol
uint8_t telegram_buffer[25];
uint8_t buffer_index = 0;

// Spike filter
float prev_t_supply  = 25.0;
float prev_t_return  = 20.0;
float prev_t_outside =  5.0;

// Simulatie
float sim_t_supply        = NAN;
float sim_t_return        = NAN;
float sim_t_outside       = NAN;
float sim_t_water_gewenst = NAN;
float sim_t_kamer         = NAN;
float sim_t_kamer_gewenst = NAN;

bool sim_actief(){ return !isnan(sim_t_supply) || !isnan(sim_t_return) || !isnan(sim_t_outside) || !isnan(sim_t_water_gewenst) || !isnan(sim_t_kamer); }

// Timers
uint8_t  discovery_fase = 0;
uint32_t vorige_discovery_ms = 0;
uint32_t vorige_data_ms = 0;
uint32_t vorige_lcd_ms = 0;
uint32_t vorige_matrix_ms = 0;
uint8_t  matrix_pagina = 0;
uint32_t vorige_pid_ms = 0;
uint32_t vorige_telegram_ms = 0;
uint32_t vorige_web_check_ms = 0;
uint32_t vorige_mqtt_rx_ms = 0;

// ═══════════════════════════════════════════════════════════════
//  MQTT LOGGING
// ═══════════════════════════════════════════════════════════════

bool mqtt_logging_enabled = true;
uint32_t laatste_log_ms = 0;
const uint32_t LOG_THROTTLE_MS = 500;

void mqtt_log(String message, String level = "INFO"){
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
//  EEPROM
// ═══════════════════════════════════════════════════════════════

#define EEPROM_MAGIC            0xAD  // v2.0: verhoogd na toevoegen FF params
#define ADDR_MAGIC              0
#define ADDR_SETPOINT           1
#define ADDR_KP                 5
#define ADDR_KI                 9
#define ADDR_KD                 13
#define ADDR_STOOKLIJN_GRENS    17
#define ADDR_STOOKLIJN_FACTOR   21
#define ADDR_T_VORST            25
#define ADDR_SUPPLY_MAX         29
#define ADDR_KOELING_MIN_BUITEN 33
#define ADDR_STOOKLIJN_UIT      37
#define ADDR_FF_UA_HOUSE        41   // nieuw v2.0
#define ADDR_FF_UA_EMITTER      45   // nieuw v2.0

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
  Serial.print("EEPROM: geladen - SP:");Serial.print(setpoint,1);
  Serial.print(" PID:");Serial.print(Kp,2);Serial.print("/");Serial.print(Ki,3);Serial.print("/");Serial.println(Kd,2);
  Serial.print("  FF UA huis:");Serial.print(ff_UA_house,0);
  Serial.print(" emitter:");Serial.println(ff_UA_emitter,0);
}

// ═══════════════════════════════════════════════════════════════
//  WARMTEPOMP PROTOCOL
// ═══════════════════════════════════════════════════════════════

uint8_t bereken_checksum(uint8_t *buf, uint8_t len){
  uint16_t sum = 0;
  for(uint8_t i=0; i<len; i++) sum += buf[i];
  return (sum & 0xFF);
}

void stuur_stand_telegram(){
  uint8_t telegram[25] = {0};
  telegram[0] = 0x19;
  telegram[1] = ctrl.stand;
  telegram[2] = 0x00;
  telegram[3] = (ctrl.stand == 0) ? 0 : (koeling_modus ? 2 : 1);
  telegram[23] = bereken_checksum(telegram, 23);
  telegram[24] = 0x00;
  chofuSerial.write(telegram, 25);
  Serial.print("TX: Stand ");Serial.print(ctrl.stand);Serial.println(" naar WP");
}

void verwerk_telegram_0x91(){
  if(telegram_buffer[0] != 0x91) return;
  uint8_t calc_cs = bereken_checksum(telegram_buffer, 23);
  if(calc_cs != telegram_buffer[23]){ Serial.println("RX: checksum fout"); return; }

  int16_t temp_raw = (telegram_buffer[3] << 8) | telegram_buffer[4];
  float new_supply = temp_raw / 10.0;
  if(abs(new_supply - prev_t_supply) > 10.0) stuur_alert("Spike aanvoer: " + String(new_supply,1));
  else { t_supply = new_supply; prev_t_supply = new_supply; }

  temp_raw = (telegram_buffer[5] << 8) | telegram_buffer[6];
  float new_return = temp_raw / 10.0;
  if(abs(new_return - prev_t_return) > 10.0) stuur_alert("Spike retour: " + String(new_return,1));
  else { t_return = new_return; prev_t_return = new_return; }

  temp_raw = (telegram_buffer[7] << 8) | telegram_buffer[8];
  float new_outside = temp_raw / 10.0;
  if(new_outside < -30.0 || new_outside > 50.0) stuur_alert("Ongeldige buitentemp: " + String(new_outside,1));
  else if(abs(new_outside - prev_t_outside) > 5.0) stuur_alert("Spike buiten: " + String(new_outside,1));
  else { t_outside = new_outside; prev_t_outside = new_outside; }

  comp_hz = telegram_buffer[9];
  pomp_snelheid_wp = telegram_buffer[10];
  defrost = (telegram_buffer[11] & 0x01);

  if(comp_hz > 0){
    werkelijk_vermogen_w = 240 + ((comp_hz - 30) / 90.0) * 1210;
    if(werkelijk_vermogen_w < 0) werkelijk_vermogen_w = 0;
    if(werkelijk_vermogen_w > 1450) werkelijk_vermogen_w = 1450;
  } else {
    werkelijk_vermogen_w = 0;
  }
  delta_t = t_supply - t_return;

  static uint8_t debug_count = 0;
  if(debug_count++ % 10 == 0){
    Serial.print("RX WP: A:");Serial.print(t_supply,1);
    Serial.print(" R:");Serial.print(t_return,1);
    Serial.print(" B:");Serial.print(t_outside,1);
    Serial.print(" Hz:");Serial.print(comp_hz);
    Serial.print(" P:");Serial.print(pomp_snelheid_wp);Serial.println("%");
  }
}

void lees_warmtepomp_data(){
  while(chofuSerial.available()){
    uint8_t byte = chofuSerial.read();
    if(byte == 0x91 || byte == 0x19){
      buffer_index = 0;
      telegram_buffer[buffer_index++] = byte;
    } else if(buffer_index > 0 && buffer_index < 25){
      telegram_buffer[buffer_index++] = byte;
      if(buffer_index == 25){
        if(telegram_buffer[0] == 0x91) verwerk_telegram_0x91();
        buffer_index = 0;
        vorige_telegram_ms = millis();
      }
    }
  }
  if(millis() - vorige_telegram_ms > 5000){
    stuur_stand_telegram();
    vorige_telegram_ms = millis();
  }
}

void pas_sim_toe(){
  if(!isnan(sim_t_supply))        { t_supply        = sim_t_supply;        prev_t_supply  = sim_t_supply; }
  if(!isnan(sim_t_return))        { t_return        = sim_t_return;        prev_t_return  = sim_t_return; }
  if(!isnan(sim_t_outside))       { t_outside       = sim_t_outside;       prev_t_outside = sim_t_outside; }
  if(!isnan(sim_t_water_gewenst)) { t_water_gewenst = sim_t_water_gewenst; }
  if(!isnan(sim_t_kamer))         { t_kamer         = sim_t_kamer; }
  if(!isnan(sim_t_kamer_gewenst)) { t_kamer_gewenst = sim_t_kamer_gewenst; }
  if(sim_actief()) delta_t = t_supply - t_return;
}

// ═══════════════════════════════════════════════════════════════
//  FEEDFORWARD REGELAAR (ff_auto / ff_water)
// ═══════════════════════════════════════════════════════════════

// COP schatting op basis van aanvoer- en buitentemperatuur (Carnot × eta=0.40)
static float ff_cop(float T_aanvoer, float T_buiten){
  float T_s = T_aanvoer + 273.15f;
  float T_o = T_buiten  + 273.15f;
  float dT  = T_s - T_o;
  if(dT < 1.0f) return 1.0f;
  return constrain(T_s / dT * 0.40f, 1.0f, 6.0f);
}

// Laagste stand waarvan VERMOGEN[s] >= P_elec (W)
static uint8_t ff_stand_voor_vermogen(float P_elec){
  for(uint8_t s = 0; s < 13; s++){
    if(VERMOGEN[s] >= P_elec) return min(s, (uint8_t)8);
  }
  return 8;
}

void pas_ff_aan(){
  bool is_water = (modus == Modus::FF_WATER);
  uint32_t nu = millis();

  // ── Stooklijn berekenen (ff_auto) / doel-aanvoer (ff_water) ──
  float stooklijn = setpoint;
  if(t_outside < STOOKLIJN_GRENS)
    stooklijn = min(45.0f, setpoint + (STOOKLIJN_GRENS - t_outside) * STOOKLIJN_FACTOR);
  // ff_water: extern opgelegd setpoint (Adam); ff_auto: stooklijn
  float wsp = is_water ? t_water_gewenst : stooklijn;
  doel_setpoint = wsp;

  // ── Buiten seizoen: WP uit (alleen ff_auto) ───────────────────
  if(!is_water && t_outside > STOOKLIJN_UIT_GRENS){
    if(ctrl.wp_aan){ ctrl.zet_uit(); }
    return;
  }

  // ── Regelafwijking ─────────────────────────────────────────────
  float regel_fout = is_water ? (wsp - t_supply) : (t_kamer_gewenst - t_kamer);
  float afschakeldrempel = is_water ? -1.5f : -1.0f;

  // ── Te warm: uitschakelen (of minimum bij vorst in auto) ───────
  if(regel_fout < afschakeldrempel){
    if(!is_water && t_outside < T_VORST && ctrl.stand > 1){
      ctrl.stand = 1; ctrl.wp_aan = true; ctrl.vorige_stand_wijz_ms = nu;
    } else {
      ctrl.zet_uit(); ctrl.vorige_stand_wijz_ms = nu;
    }
    return;
  }

  // ── Feedforward: benodigde warmte ─────────────────────────────
  // ff_water: emitter-model — hoeveel thermisch vermogen nodig om t_water_gewenst te halen
  // ff_auto:  huis-model   — hoeveel warmte het huis vraagt op basis van kamertemp
  float P_nodig;
  float cop;
  if(is_water){
    P_nodig = max(0.0f, ff_UA_emitter * (t_water_gewenst - t_kamer));
    cop = ff_cop(max(t_water_gewenst, t_kamer + 1.0f), t_outside);
  } else {
    P_nodig = max(0.0f, ff_UA_house * (t_kamer_gewenst - t_outside));
    cop = ff_cop(t_supply, t_outside);
  }

  // Onderhoudstand: laagste stand met voldoende elektrisch vermogen
  uint8_t stand_ff = ff_stand_voor_vermogen(P_nodig / max(cop, 1.0f));

  // Anticipatiezone: +1 bij grote fout (drempel verschilt per modus)
  float coast_k = is_water ? FF_COAST_WATER : FF_COAST_AUTO;
  if(regel_fout > coast_k) stand_ff = min(8, (int)stand_ff + 1);

  // ── Integraalcorrectie (±2 stand, traag) ──────────────────────
  float ff_ki = is_water ? FF_KI_WATER : FF_KI_AUTO;
  ctrl.ff_integraal += regel_fout * (pid_interval_ms / 1000.0f);
  float ff_max_int = 3.0f * 3600.0f / ff_ki;
  ctrl.ff_integraal = constrain(ctrl.ff_integraal, -ff_max_int, ff_max_int);
  int8_t int_corr = (int8_t)constrain((int)(ctrl.ff_integraal * ff_ki / 3600.0f), -2, 2);
  // Boven setpoint: integraal mag stand niet bóven equilibrium houden — eerst terugregelen
  if(regel_fout <= 0.0f && int_corr > 0) int_corr = 0;

  // Stapgrootte: groter bij grote fout zodat systeem snel op niveau komt
  int max_stap = (regel_fout > 3.0f) ? 3 : 1;

  // Nieuwe stand: FF + correctie, met stapbeperking
  int nieuwe_stand_i = constrain((int)stand_ff + int_corr, 0, 8);
  if(!is_water && t_outside < T_VORST) nieuwe_stand_i = max(1, nieuwe_stand_i);
  nieuwe_stand_i = constrain(nieuwe_stand_i, max(0, (int)ctrl.stand - max_stap), min(8, (int)ctrl.stand + max_stap));
  uint8_t nieuwe_stand = (uint8_t)nieuwe_stand_i;

  // ── Online leren ───────────────────────────────────────────────
  // Alleen bij thermisch evenwicht (klein setpoint-fout): anders absorbeert
  // het model de thermische-massa term (C_th × dT/dt) als extra UA.
  float leer_drempel = is_water ? 2.0f : 0.5f;
  float P_hp_est = VERMOGEN[ctrl.stand] * ff_cop(t_supply, t_outside);
  if(ctrl.stand > 0 && P_hp_est > 50.0f && fabsf(regel_fout) < leer_drempel){
    if(is_water){
      float dt_sup = t_supply - t_kamer;
      if(dt_sup > 2.0f){
        float meting = P_hp_est / dt_sup;
        ff_UA_emitter = (1.0f - FF_LEARN_RATE) * ff_UA_emitter + FF_LEARN_RATE * meting;
        ff_UA_emitter = constrain(ff_UA_emitter, 50.0f, 500.0f);
      }
    } else {
      float dt_env = t_kamer - t_outside;
      if(dt_env > 3.0f){
        float meting = P_hp_est / dt_env;
        ff_UA_house = (1.0f - FF_LEARN_RATE) * ff_UA_house + FF_LEARN_RATE * meting;
        ff_UA_house = constrain(ff_UA_house, 50.0f, 500.0f);
      }
    }
  }

  // pid_output tonen als stand_ff × 12.5 (zodat HA entity 0-100% klopt)
  ctrl.pid_output = stand_ff * 12.5f;

  // ── Hysteresis ─────────────────────────────────────────────────
  long hyst;
  if(nieuwe_stand < ctrl.stand)  hyst = HYST_DOWN_MS;
  else if(regel_fout > coast_k)  hyst = HYST_FAST_MS;
  else                           hyst = HYST_SLOW_MS;

  // Debug: altijd printen zodat we kunnen zien waarom stand niet verandert
  Serial.print("FF dbg: kgew="); Serial.print(t_kamer_gewenst,1);
  Serial.print(" kamer="); Serial.print(t_kamer,1);
  Serial.print(" buiten="); Serial.print(t_outside,1);
  Serial.print(" fout="); Serial.print(regel_fout,2);
  Serial.print(" UA="); Serial.print(ff_UA_house,0);
  Serial.print(" Pnodig="); Serial.print((int)(ff_UA_house * max(0.0f, t_kamer_gewenst - t_outside)));
  Serial.print("W ff="); Serial.print(stand_ff);
  Serial.print(" maxstap="); Serial.print(max_stap);
  Serial.print(" nieuw="); Serial.print(nieuwe_stand);
  Serial.print(" hyst="); Serial.print(hyst/1000);
  Serial.print("s elapsed="); Serial.print((nu - ctrl.vorige_stand_wijz_ms)/1000);
  Serial.println("s");

  if(nieuwe_stand != ctrl.stand && (nu - ctrl.vorige_stand_wijz_ms >= (uint32_t)hyst)){
    ctrl.stand = nieuwe_stand;
    ctrl.vorige_stand_wijz_ms = nu;
    ctrl.wp_aan = (ctrl.stand > 0);
    if(ctrl.stand == 0) ctrl.reset_ff();
    String s = is_water ? "FF-W" : "FF-A";
    mqtt_log(s + ": A=" + String(t_supply,1) +
             " fout=" + String(regel_fout,1) +
             " ff=" + String(stand_ff) +
             " cor=" + String(int_corr) +
             " UA=" + String(is_water ? ff_UA_emitter : ff_UA_house, 0) +
             " St=" + String(ctrl.stand), "INFO");
  }
}

// ═══════════════════════════════════════════════════════════════
//  PID REGELING (auto / water)
// ═══════════════════════════════════════════════════════════════

void pas_pid_aan(){
  // Safeguards (altijd, ongeacht modus)
  if(t_supply > SUPPLY_MAX){
    ctrl.zet_uit();
    stuur_alert("NOODSTOP aanvoer: " + String(t_supply,1) + "C > max " + String(SUPPLY_MAX,1) + "C");
    return;
  }
  if(koeling_modus && t_outside < KOELING_MIN_BUITEN){
    koeling_modus = false; ctrl.reset_pid();
    stuur_alert("Koeling geblokkeerd: buiten " + String(t_outside,1) + "C");
  }

  if(modus == Modus::HANDMATIG){
    ctrl.stand = handmatig_stand;
    ctrl.wp_aan = (ctrl.stand > 0);
    return;
  }

  // FF modi — eigen regelaar
  if(modus == Modus::FF_AUTO || modus == Modus::FF_WATER){
    uint32_t nu = millis();
    if(nu - vorige_pid_ms < (uint32_t)pid_interval_ms) return;
    vorige_pid_ms = nu;
    pas_ff_aan();
    return;
  }

  uint32_t nu = millis();
  if(nu - vorige_pid_ms < (uint32_t)pid_interval_ms) return;
  vorige_pid_ms = nu;

  // ── WATER MODUS ────────────────────────────────────────────────
  if(modus == Modus::WATER){
    float water_fout = koeling_modus ? (t_supply - t_water_gewenst)
                                     : (t_water_gewenst - t_supply);
    if(t_outside < T_VORST && ctrl.stand == 0){
      ctrl.stand = 1; ctrl.wp_aan = true; ctrl.vorige_stand_wijz_ms = nu;
      mqtt_log("VORSTBEVEILIGING buiten: " + String(t_outside,1) + "C", "WARNING");
    }
    if(water_fout > 1.0){
      ctrl.wp_aan = true;
    } else if(water_fout < -1.0){
      if(t_outside >= T_VORST){
        ctrl.zet_uit();
        mqtt_log("WATER: setpoint bereikt -> WP UIT", "INFO");
      }
      return;
    }
    if(ctrl.wp_aan){
      ctrl.pid_integraal += water_fout * 0.005;
      ctrl.pid_integraal = constrain(ctrl.pid_integraal, -50.0f, 50.0f);
      float diff = (water_fout - ctrl.pid_vorige_fout) / 0.005;
      ctrl.pid_vorige_fout = water_fout;
      ctrl.pid_output = constrain(Kp * water_fout + Ki * ctrl.pid_integraal + Kd * diff, -100.0f, 100.0f);

      uint8_t nieuwe_stand = 0;
      if(ctrl.pid_output < 5)        nieuwe_stand = 0;
      else if(ctrl.pid_output < 15)  nieuwe_stand = 1;
      else if(ctrl.pid_output < 25)  nieuwe_stand = 2;
      else if(ctrl.pid_output < 40)  nieuwe_stand = 3;
      else if(ctrl.pid_output < 55)  nieuwe_stand = 4;
      else if(ctrl.pid_output < 70)  nieuwe_stand = 5;
      else if(ctrl.pid_output < 85)  nieuwe_stand = 6;
      else if(ctrl.pid_output < 93)  nieuwe_stand = 7;
      else                           nieuwe_stand = 8;

      if(t_outside < T_VORST && nieuwe_stand == 0) nieuwe_stand = 1;
      long hyst = (nieuwe_stand < ctrl.stand) ? HYST_DOWN_MS :
                  (water_fout > 5.0)          ? HYST_FAST_MS : HYST_SLOW_MS;
      if(nieuwe_stand != ctrl.stand && (nu - ctrl.vorige_stand_wijz_ms >= (uint32_t)hyst)){
        ctrl.stand = nieuwe_stand; ctrl.vorige_stand_wijz_ms = nu;
        if(ctrl.stand == 0){ ctrl.wp_aan = false; ctrl.reset_pid(); }
        mqtt_log("WATER: A=" + String(t_supply,1) + " fout=" + String(water_fout,1) + " St=" + String(ctrl.stand), "INFO");
      }
    }
    return;
  }

  // ── AUTO MODUS ─────────────────────────────────────────────────
  if(t_outside > STOOKLIJN_UIT_GRENS){
    if(ctrl.wp_aan){ ctrl.zet_uit();
      stuur_alert("Verwarming gestopt: buiten " + String(t_outside,1) + "C"); }
    return;
  }
  float kamer_fout = t_kamer_gewenst - t_kamer;
  doel_setpoint = setpoint;
  if(t_outside < STOOKLIJN_GRENS){
    doel_setpoint = min(45.0f, setpoint + (STOOKLIJN_GRENS - t_outside) * STOOKLIJN_FACTOR);
  }
  if(t_outside < T_VORST && ctrl.stand == 0){
    ctrl.stand = 1; ctrl.wp_aan = true; ctrl.vorige_stand_wijz_ms = nu;
    mqtt_log("VORSTBEVEILIGING buiten: " + String(t_outside,1) + "C", "WARNING");
  }
  float absolute_max = min(t_kamer_gewenst + 0.5f, 25.0f);
  if(t_kamer > absolute_max){
    ctrl.zet_uit();
    mqtt_log("MAX! Kamer: " + String(t_kamer,1) + "C", "ERROR");
    return;
  }
  if(kamer_fout > 0.1f){
    ctrl.wp_aan = true;
    float aanvoer_fout = doel_setpoint - t_supply;
    float dt_correctie = 0;
    if(delta_t < 4.0)      dt_correctie = (delta_t - 5.0) * 3.0;
    else if(delta_t > 6.0) dt_correctie = (delta_t - 5.0) * 2.0;
    float kamer_correctie = kamer_fout * (kamer_fout > 1.5f ? 30.0f : 20.0f);

    float diff = (aanvoer_fout - ctrl.pid_vorige_fout) / 0.005;
    ctrl.pid_vorige_fout = aanvoer_fout;
    float pid_output_raw = Kp * aanvoer_fout + Ki * ctrl.pid_integraal + Kd * diff + dt_correctie + kamer_correctie;
    // Anti-windup: integreer alleen als output niet gesatureerd is in de windrichting
    if(!((pid_output_raw > 100.0f && aanvoer_fout > 0.0f) ||
         (pid_output_raw <   0.0f && aanvoer_fout < 0.0f))){
      ctrl.pid_integraal += aanvoer_fout * 0.005;
      ctrl.pid_integraal = constrain(ctrl.pid_integraal, -50.0f, 50.0f);
    }
    ctrl.pid_output = pid_output_raw;
    if(kamer_fout > 1.5f && ctrl.pid_output < 55.0f) ctrl.pid_output = 55.0f;
    ctrl.pid_output = constrain(ctrl.pid_output, 0.0f, 100.0f);

    uint8_t nieuwe_stand = 0;
    if(ctrl.pid_output < 5)        nieuwe_stand = 0;
    else if(ctrl.pid_output < 15)  nieuwe_stand = 1;
    else if(ctrl.pid_output < 25)  nieuwe_stand = 2;
    else if(ctrl.pid_output < 40)  nieuwe_stand = 3;
    else if(ctrl.pid_output < 55)  nieuwe_stand = 4;
    else if(ctrl.pid_output < 70)  nieuwe_stand = 5;
    else if(ctrl.pid_output < 85)  nieuwe_stand = 6;
    else if(ctrl.pid_output < 93)  nieuwe_stand = 7;
    else                           nieuwe_stand = 8;

    if(t_outside < T_VORST && nieuwe_stand == 0) nieuwe_stand = 1;
    long hyst = (kamer_fout > 1.0f) ? HYST_FAST_MS : HYST_SLOW_MS;
    if(nieuwe_stand != ctrl.stand && (nu - ctrl.vorige_stand_wijz_ms >= (uint32_t)hyst)){
      ctrl.stand = nieuwe_stand; ctrl.vorige_stand_wijz_ms = nu;
      Serial.print("AUTO: kamer=");Serial.print(t_kamer,1);
      Serial.print(" fout=");Serial.print(kamer_fout,1);
      Serial.print(" St=");Serial.println(ctrl.stand);
    }
  } else if(kamer_fout < -0.2f){
    if(t_outside < T_VORST){
      if(ctrl.stand > 1){ ctrl.stand = 1; ctrl.wp_aan = true; }
    } else {
      ctrl.zet_uit();
      mqtt_log("Kamer te warm (" + String(t_kamer,1) + "C) -> WP UIT", "INFO");
    }
  }
}

// ═══════════════════════════════════════════════════════════════
//  MQTT FUNCTIES
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

void mqtt_ontvang(int len){
  vorige_mqtt_rx_ms = millis();
  String topic = mqttClient.messageTopic();
  String payload = "";
  while(mqttClient.available()) payload += (char)mqttClient.read();
  Serial.print("MQTT: ");Serial.print(topic);Serial.print("=");Serial.println(payload);

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
  else if(topic == "chofu/cmd/setpoint"){
    float val = payload.toFloat();
    if(val >= 20 && val <= 45){ setpoint = val; eeprom_save(); }
  }
  else if(topic == "chofu/cmd/kp"){ Kp = payload.toFloat(); eeprom_save(); }
  else if(topic == "chofu/cmd/ki"){ Ki = payload.toFloat(); eeprom_save(); }
  else if(topic == "chofu/cmd/kd"){ Kd = payload.toFloat(); eeprom_save(); }
  else if(topic == "chofu/cmd/modus"){
    if(payload == "auto" || payload == "handmatig" || payload == "water" ||
       payload == "ff_auto" || payload == "ff_water"){
      modus = str_naar_modus(payload);
      if(modus != Modus::HANDMATIG){ ctrl.reset_pid(); ctrl.reset_ff(); }
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
    koeling_modus = (payload == "1"); ctrl.reset_pid();
    mqtt_log(koeling_modus ? "Koeling aan" : "Verwarming aan", "INFO");
  }
  else if(topic == "chofu/cmd/water_setpoint"){
    float val = payload.toFloat();
    if(val >= 25 && val <= 55){ t_water_gewenst = val;
      mqtt_log("Water SP: " + String(t_water_gewenst,1) + "C", "INFO"); }
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
  // FF UA waarden instellen/overschrijven
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
  // FF UA handmatig opslaan (bijv. na langere leerperiode)
  else if(topic == "chofu/cmd/ff_save"){
    eeprom_save();
    mqtt_log("FF UA opgeslagen: huis=" + String(ff_UA_house,0) + " emitter=" + String(ff_UA_emitter,0), "INFO");
  }
  else if(topic == "anna/setpoint"){
    float val = payload.toFloat();
    if(val >= 14 && val <= 30) t_kamer_gewenst = val;
  }
  else if(topic == "chofu/cmd/kamer_setpoint"){
    float val = payload.toFloat();
    if(val >= 14 && val <= 25){
      t_kamer_gewenst = val;
      mqttClient.beginMessage("chofu/kamer_gewenst"); mqttClient.print(t_kamer_gewenst, 1); mqttClient.endMessage();
    }
  }
  else if(topic == "anna/temperatuur"){
    float val = payload.toFloat();
    if(val >= 5 && val <= 35) t_kamer = val;
  }
  else if(topic == "chofu/cmd/force_start"){
    ctrl.vorige_stand_wijz_ms = 0;
    Serial.println("FORCE START - hysteresis gereset");
  }
  // Simulatie topics
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
    else { float v = payload.toFloat(); if(v >= 25 && v <= 55) sim_t_water_gewenst = v; }
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

  if (!topic.startsWith("chofu/sim/")) stuur_data();
}

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
  pl = "{\"name\":\"Chofu Kamer\",\"uniq_id\":\"chofu_hp_kamer\",\"stat_t\":\"chofu/kamer\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/chofu_hp/kamer/config", pl);
  pl = "{\"name\":\"Chofu Kamer Gewenst\",\"uniq_id\":\"chofu_hp_kamer_gewenst\",\"cmd_t\":\"chofu/cmd/kamer_setpoint\",\"stat_t\":\"chofu/kamer_gewenst\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"min\":14,\"max\":25,\"step\":0.5," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/chofu_hp/kamer_gewenst/config", pl);
  pl = "{\"name\":\"Chofu Setpoint\",\"uniq_id\":\"chofu_hp_setpoint\",\"stat_t\":\"chofu/setpoint\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/chofu_hp/setpoint/config", pl);
  pl = "{\"name\":\"Chofu Vorstgrens\",\"uniq_id\":\"chofu_hp_t_vorst\",\"cmd_t\":\"chofu/cmd/t_vorst\",\"stat_t\":\"chofu/t_vorst\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"min\":-10,\"max\":10,\"step\":0.5," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/chofu_hp/t_vorst/config", pl);
  pl = "{\"name\":\"Chofu Water SP\",\"uniq_id\":\"chofu_hp_water_setpoint\",\"cmd_t\":\"chofu/cmd/water_setpoint\",\"stat_t\":\"chofu/water_setpoint\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"min\":25,\"max\":55,\"step\":0.5," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/chofu_hp/water_setpoint/config", pl);
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

  // Modus select — inclusief FF modi
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

  // FF UA sensors en instelbare getallen
  pl = "{\"name\":\"Chofu FF UA Huis\",\"uniq_id\":\"chofu_hp_ff_ua_house\",\"stat_t\":\"chofu/ff_ua_house\",\"unit_of_meas\":\"W/K\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/chofu_hp/ff_ua_house/config", pl);
  pl = "{\"name\":\"Chofu FF UA Emitter\",\"uniq_id\":\"chofu_hp_ff_ua_emitter\",\"stat_t\":\"chofu/ff_ua_emitter\",\"unit_of_meas\":\"W/K\"," + avty + "," + dev + "}";
  disco_pub("homeassistant/sensor/chofu_hp/ff_ua_emitter/config", pl);
  pl = "{\"name\":\"Chofu FF UA Huis instellen\",\"uniq_id\":\"chofu_hp_ff_ua_house_cmd\",\"cmd_t\":\"chofu/cmd/ff_ua_house\",\"stat_t\":\"chofu/ff_ua_house\",\"unit_of_meas\":\"W/K\",\"min\":50,\"max\":500,\"step\":1," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/chofu_hp/ff_ua_house_cmd/config", pl);
  pl = "{\"name\":\"Chofu FF UA Emitter instellen\",\"uniq_id\":\"chofu_hp_ff_ua_emitter_cmd\",\"cmd_t\":\"chofu/cmd/ff_ua_emitter\",\"stat_t\":\"chofu/ff_ua_emitter\",\"unit_of_meas\":\"W/K\",\"min\":50,\"max\":500,\"step\":1," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/chofu_hp/ff_ua_emitter_cmd/config", pl);

  // Simulatie timing
  pl = "{\"name\":\"Chofu Hyst Slow ms\",\"uniq_id\":\"chofu_hp_hyst_slow\",\"cmd_t\":\"chofu/cmd/hyst_slow\",\"stat_t\":\"chofu/hyst_slow\",\"unit_of_meas\":\"ms\",\"min\":100,\"max\":3600000,\"step\":1000," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/chofu_hp/hyst_slow/config", pl);
  pl = "{\"name\":\"Chofu Hyst Fast ms\",\"uniq_id\":\"chofu_hp_hyst_fast\",\"cmd_t\":\"chofu/cmd/hyst_fast\",\"stat_t\":\"chofu/hyst_fast\",\"unit_of_meas\":\"ms\",\"min\":100,\"max\":3600000,\"step\":1000," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/chofu_hp/hyst_fast/config", pl);
  pl = "{\"name\":\"Chofu Hyst Down ms\",\"uniq_id\":\"chofu_hp_hyst_down\",\"cmd_t\":\"chofu/cmd/hyst_down\",\"stat_t\":\"chofu/hyst_down\",\"unit_of_meas\":\"ms\",\"min\":100,\"max\":3600000,\"step\":1000," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/chofu_hp/hyst_down/config", pl);
  pl = "{\"name\":\"Chofu PID Interval ms\",\"uniq_id\":\"chofu_hp_pid_interval\",\"cmd_t\":\"chofu/cmd/pid_interval\",\"stat_t\":\"chofu/pid_interval\",\"unit_of_meas\":\"ms\",\"min\":100,\"max\":60000,\"step\":100," + avty + "," + dev + "}";
  disco_pub("homeassistant/number/chofu_hp/pid_interval/config", pl);

  stuur_data();
}

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
  mqttClient.beginMessage("chofu/setpoint");mqttClient.print(setpoint,1);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/doel_setpoint");mqttClient.print(doel_setpoint,1);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/stooklijn_grens");mqttClient.print(STOOKLIJN_GRENS,1);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/stooklijn_factor");mqttClient.print(STOOKLIJN_FACTOR,2);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/water_setpoint");mqttClient.print(t_water_gewenst,1);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/koeling");mqttClient.print(koeling_modus?"1":"0");mqttClient.endMessage();
  mqttClient.beginMessage("chofu/t_vorst");mqttClient.print(T_VORST,1);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/modus");mqttClient.print(modus_naar_str(modus));mqttClient.endMessage();
  mqttClient.beginMessage("chofu/lcd");mqttClient.print(lcd_enabled?"1":"0");mqttClient.endMessage();
  mqttClient.beginMessage("chofu/defrost");mqttClient.print(defrost?"1":"0");mqttClient.endMessage();
  mqttClient.beginMessage("chofu/pid");mqttClient.print(ctrl.pid_output,1);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/pomp");mqttClient.print(pomp_snelheid_wp);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/comp_hz");mqttClient.print(comp_hz);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/sim_actief");mqttClient.print(sim_actief()?"1":"0");mqttClient.endMessage();
  mqttClient.beginMessage("chofu/supply_max");mqttClient.print(SUPPLY_MAX,1);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/koeling_min_buiten");mqttClient.print(KOELING_MIN_BUITEN,1);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/stooklijn_uit");mqttClient.print(STOOKLIJN_UIT_GRENS,1);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/hyst_slow");mqttClient.print(HYST_SLOW_MS);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/hyst_fast");mqttClient.print(HYST_FAST_MS);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/hyst_down");mqttClient.print(HYST_DOWN_MS);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/pid_interval");mqttClient.print(pid_interval_ms);mqttClient.endMessage();
  // FF leerwaarden
  mqttClient.beginMessage("chofu/ff_ua_house");mqttClient.print(ff_UA_house,1);mqttClient.endMessage();
  mqttClient.beginMessage("chofu/ff_ua_emitter");mqttClient.print(ff_UA_emitter,1);mqttClient.endMessage();

  // Ververs retained availability zodat HA entiteiten beschikbaar blijven na discovery
  mqttClient.beginMessage("chofu/status", true, 1);
  mqttClient.print("online");
  mqttClient.endMessage();

  vorige_data_ms = millis();
}

// ═══════════════════════════════════════════════════════════════
//  WEB INTERFACE
// ═══════════════════════════════════════════════════════════════

void handle_web_client(){
  if(millis() - vorige_web_check_ms < 100) return;
  vorige_web_check_ms = millis();
  WiFiClient client = webServer.available();
  if(!client) return;
  Serial.println("Web client verbonden");
  String request = "";
  uint32_t web_timeout = millis();
  while(client.connected() && millis() - web_timeout < 2000){
    if(client.available()){
      char c = client.read(); request += c;
      if(c == '\n' && request.endsWith("\r\n\r\n")) break;
    }
  }
  if(!client.connected() && request.length() == 0){ client.stop(); return; }
  if(request.indexOf("GET /?") >= 0){
    auto parse_param = [&](const char* key) -> String {
      String k = String(key) + "=";
      int idx = request.indexOf(k);
      if(idx < 0) return "";
      idx += k.length();
      int end1 = request.indexOf("&", idx);
      int end2 = request.indexOf(" ", idx);
      int end = (end1 > 0 && end1 < end2) ? end1 : end2;
      return request.substring(idx, end);
    };
    String v;
    v = parse_param("setpoint");   if(v.length()){ setpoint = v.toFloat(); eeprom_save(); }
    v = parse_param("kp");         if(v.length()){ Kp = v.toFloat(); eeprom_save(); }
    v = parse_param("ki");         if(v.length()){ Ki = v.toFloat(); eeprom_save(); }
    v = parse_param("kd");         if(v.length()){ Kd = v.toFloat(); eeprom_save(); }
    v = parse_param("modus");      if(v.length() && (v=="auto"||v=="water"||v=="ff_auto"||v=="ff_water"||v=="handmatig")){
      modus = str_naar_modus(v); if(modus != Modus::HANDMATIG){ ctrl.reset_pid(); ctrl.reset_ff(); } }
    v = parse_param("water_setpoint"); if(v.length()){ float f=v.toFloat(); if(f>=25&&f<=55) t_water_gewenst=f; }
    v = parse_param("ff_ua_house");    if(v.length()){ float f=v.toFloat(); if(f>=50&&f<=500){ ff_UA_house=f; eeprom_save(); } }
    v = parse_param("ff_ua_emitter");  if(v.length()){ float f=v.toFloat(); if(f>=50&&f<=500){ ff_UA_emitter=f; eeprom_save(); } }
  }

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html");
  client.println();
  client.println("<!DOCTYPE html><html><head>");
  client.println("<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>");
  client.println("<title>Kromhout WP v2.0</title>");
  client.println("<style>body{font-family:Arial;margin:20px;background:#f0f0f0}h1{color:#2c3e50}.card{background:white;padding:20px;margin:10px 0;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1)}.temp{font-size:24px;font-weight:bold;color:#e74c3c}input,select{padding:8px;margin:5px;border:1px solid #ccc;border-radius:4px}button{background:#3498db;color:white;padding:10px 20px;border:none;border-radius:4px;cursor:pointer}button:hover{background:#2980b9}.status{display:inline-block;width:12px;height:12px;border-radius:50%;margin-right:5px}.on{background:#27ae60}.off{background:#95a5a6}.ff{background:#8e44ad}</style></head><body>");
  client.println("<h1>Kromhout Warmtepomp v2.0</h1>");

  // Status
  client.println("<div class='card'><h2>Status</h2>");
  client.print("<div><span class='status "); client.print(ctrl.wp_aan?"on":"off"); client.print("'></span>WP: <b>"); client.print(ctrl.wp_aan?"AAN":"UIT"); client.println("</b></div>");
  client.print("<div>Modus: <b>"); client.print(modus_naar_str(modus)); client.println("</b></div>");
  client.print("<div>Stand: <b>"); client.print(ctrl.stand); client.print("</b> ("); client.print(VERMOGEN[ctrl.stand]); client.println(" W)</div>");
  if(modus == Modus::FF_AUTO || modus == Modus::FF_WATER){
    client.print("<div>FF UA huis: <b>"); client.print(ff_UA_house,0); client.println(" W/K</b></div>");
    client.print("<div>FF UA emitter: <b>"); client.print(ff_UA_emitter,0); client.println(" W/K</b></div>");
  }
  client.println("</div>");

  // Temperaturen
  client.println("<div class='card'><h2>Temperaturen</h2>");
  client.print("<div>Aanvoer: <span class='temp'>"); client.print(t_supply,1); client.println("°C</span></div>");
  client.print("<div>Retour: <span class='temp'>"); client.print(t_return,1); client.println("°C</span></div>");
  client.print("<div>Delta T: <span class='temp'>"); client.print(delta_t,1); client.println("°C</span></div>");
  client.print("<div>Buiten: <span class='temp'>"); client.print(t_outside,1); client.println("°C</span></div>");
  client.print("<div>Kamer: <span class='temp'>"); client.print(t_kamer,1); client.print("°C</span> → "); client.print(t_kamer_gewenst,1); client.println("°C</div>");
  client.println("</div>");

  // Instellingen
  client.println("<div class='card'><h2>Instellingen</h2><form>");
  client.print("<div>Setpoint: <input type='number' name='setpoint' value='"); client.print(setpoint,1); client.println("' step='0.5' min='20' max='45'> °C</div>");
  client.print("<div>Modus: <select name='modus'>");
  for(const char* m : {"auto","water","ff_auto","ff_water","handmatig"}){
    client.print("<option value='"); client.print(m); client.print("'");
    if(strcmp(modus_naar_str(modus), m) == 0) client.print(" selected");
    client.print(">"); client.print(m); client.println("</option>");
  }
  client.println("</select></div>");
  client.print("<div>Water setpoint: <input type='number' name='water_setpoint' value='"); client.print(t_water_gewenst,1); client.println("' step='0.5' min='25' max='55'> °C</div>");
  client.println("<h3>PID Parameters</h3>");
  client.print("<div>Kp: <input type='number' name='kp' value='"); client.print(Kp,2); client.println("' step='0.1' min='0' max='10'></div>");
  client.print("<div>Ki: <input type='number' name='ki' value='"); client.print(Ki,3); client.println("' step='0.001' min='0' max='1'></div>");
  client.print("<div>Kd: <input type='number' name='kd' value='"); client.print(Kd,2); client.println("' step='0.1' min='0' max='10'></div>");
  client.println("<h3>FF Parameters (lerende UA)</h3>");
  client.print("<div>UA huis: <input type='number' name='ff_ua_house' value='"); client.print(ff_UA_house,0); client.println("' step='1' min='50' max='500'> W/K</div>");
  client.print("<div>UA emitter: <input type='number' name='ff_ua_emitter' value='"); client.print(ff_UA_emitter,0); client.println("' step='1' min='50' max='500'> W/K</div>");
  client.print("<div>PID output: <b>"); client.print(ctrl.pid_output,1); client.println("%</b></div>");
  client.println("<br><button type='submit'>Opslaan</button></form></div>");

  client.print("<div class='card'><small>IP: "); client.print(WiFi.localIP());
  client.print(" | Uptime: "); client.print(millis()/1000/60); client.println(" min</small></div>");
  client.println("<script>setTimeout(function(){location.reload()},10000);</script>");
  client.println("</body></html>");
  delay(10);
  client.stop();
}

// ═══════════════════════════════════════════════════════════════
//  LCD
// ═══════════════════════════════════════════════════════════════

void update_lcd(){
  if(!USE_LCD || !lcd_enabled) return;
  uint32_t nu = millis();
  if(nu - vorige_lcd_ms < 6000) return; // elke 6 sec
  vorige_lcd_ms = nu;

  static uint8_t scherm = 0;
  lcd.clear();
  int verm = (werkelijk_vermogen_w > 0) ? (int)werkelijk_vermogen_w : VERMOGEN[ctrl.stand];

  switch(scherm){
    case 0:
      lcd.print("St");lcd.print(ctrl.stand);
      lcd.print(" ");lcd.print(verm);lcd.print("W");
      lcd.print(ctrl.wp_aan?" ON":" OFF");
      lcd.setCursor(0,1);
      if(modus==Modus::AUTO)       lcd.print("AUTO");
      else if(modus==Modus::WATER) lcd.print("WATR");
      else if(modus==Modus::FF_AUTO)  lcd.print("FF-A");
      else if(modus==Modus::FF_WATER) lcd.print("FF-W");
      else                         lcd.print("HAND");
      lcd.print(" Hz:");lcd.print(comp_hz);
      break;
    case 1:
      lcd.print("A:");lcd.print(t_supply,1);
      lcd.print(" R:");lcd.print(t_return,1);
      lcd.setCursor(0,1);
      lcd.print("DT:");lcd.print(delta_t,1);
      if(modus==Modus::WATER||modus==Modus::FF_WATER){
        lcd.print(" W:");lcd.print(t_water_gewenst,0);
      } else {
        lcd.print(" D:");lcd.print(doel_setpoint,0);
      }
      break;
    case 2:
      lcd.print("T:");lcd.print(t_kamer,1);
      lcd.print(" Doel:");lcd.print(t_kamer_gewenst,1);
      lcd.setCursor(0,1);
      lcd.print("B:");lcd.print(t_outside,1);
      if(modus==Modus::FF_AUTO||modus==Modus::FF_WATER){
        lcd.print(" UA:");lcd.print(modus==Modus::FF_WATER ? ff_UA_emitter : ff_UA_house, 0);
      }
      break;
    case 3:
      lcd.print("PID:");lcd.print(ctrl.pid_output,0);lcd.print("% ");
      lcd.print("P:");lcd.print(pomp_snelheid_wp);
      lcd.setCursor(0,1);
      lcd.print(WiFi.localIP());
      break;
  }
  scherm = (scherm + 1) % 4;
}

// ═══════════════════════════════════════════════════════════════
//  LED MATRIX
// ═══════════════════════════════════════════════════════════════

void update_matrix() {
  if(!USE_LED_MATRIX) return;
  uint32_t nu = millis();
  if(nu - vorige_matrix_ms < 4000) return;
  vorige_matrix_ms = nu;

  // Pagina 0 — stand bar: elke kolom = één stand (0–12)
  // Pagina 1 — status icoon: vlam / sneeuwvlok / druppel (defrost) / cirkel (uit)
  static const uint8_t ICON_FLAME[8][12] = {
    {0,0,0,0,0,1,0,0,0,0,0,0},
    {0,0,0,0,1,1,1,0,0,0,0,0},
    {0,0,0,1,0,1,0,1,0,0,0,0},
    {0,0,0,1,1,1,1,1,0,0,0,0},
    {0,0,1,1,1,1,1,1,1,0,0,0},
    {0,0,1,1,1,0,1,1,1,0,0,0},
    {0,0,0,1,1,1,1,1,0,0,0,0},
    {0,0,0,0,1,1,1,0,0,0,0,0},
  };
  static const uint8_t ICON_SNOW[8][12] = {
    {0,0,0,0,0,1,0,0,0,0,0,0},
    {0,1,0,0,0,1,0,0,0,1,0,0},
    {0,0,1,0,0,1,0,0,1,0,0,0},
    {0,0,0,1,0,1,0,1,0,0,0,0},
    {1,1,1,1,1,1,1,1,1,1,1,1},
    {0,0,0,1,0,1,0,1,0,0,0,0},
    {0,0,1,0,0,1,0,0,1,0,0,0},
    {0,1,0,0,0,1,0,0,0,1,0,0},
  };
  static const uint8_t ICON_DEFROST[8][12] = {
    {0,0,0,0,0,1,0,0,0,0,0,0},
    {0,0,0,0,1,1,1,0,0,0,0,0},
    {0,0,0,0,1,1,1,0,0,0,0,0},
    {0,0,0,0,0,1,0,0,0,0,0,0},
    {0,0,0,0,0,1,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,1,0,0,0,0,0,0},
    {0,0,0,0,1,1,1,0,0,0,0,0},
  };
  static const uint8_t ICON_OFF[8][12] = {
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,1,1,1,0,0,0,0,0},
    {0,0,0,1,0,0,0,1,0,0,0,0},
    {0,0,0,1,0,0,0,1,0,0,0,0},
    {0,0,0,0,1,1,1,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
  };

  // Pagina 2 — modus icoon: cirkel=auto, drie lijnen=ff_auto, druppel=water, druppelrand=ff_water, pijlen=handmatig
  static const uint8_t MODUS_AUTO[8][12] = {
    {0,0,0,0,1,1,1,0,0,0,0,0},
    {0,0,0,1,0,0,0,1,0,0,0,0},
    {0,0,1,0,0,0,0,0,1,0,0,0},
    {0,0,1,0,0,0,0,0,1,0,0,0},
    {0,0,1,0,0,0,0,0,1,0,0,0},
    {0,0,0,1,0,0,0,1,0,0,0,0},
    {0,0,0,0,1,1,1,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
  };
  static const uint8_t MODUS_FF_AUTO[8][12] = {
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,1,1,1,1,1,1,1,1,1,1,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,1,1,1,1,1,1,1,1,1,1,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,1,1,1,1,1,1,1,1,1,1,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
  };
  static const uint8_t MODUS_WATER[8][12] = {
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,1,0,0,0,0,0,0},
    {0,0,0,0,1,1,1,0,0,0,0,0},
    {0,0,0,1,1,1,1,1,0,0,0,0},
    {0,0,1,1,1,1,1,1,1,0,0,0},
    {0,0,0,1,1,1,1,1,0,0,0,0},
    {0,0,0,0,1,1,1,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
  };
  static const uint8_t MODUS_FF_WATER[8][12] = {
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,1,0,0,0,0,0,0},
    {0,0,0,0,1,0,1,0,0,0,0,0},
    {0,0,0,1,0,0,0,1,0,0,0,0},
    {0,0,1,0,0,0,0,0,1,0,0,0},
    {0,0,0,1,0,0,0,1,0,0,0,0},
    {0,0,0,0,1,1,1,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
  };
  static const uint8_t MODUS_HAND[8][12] = {
    {0,0,0,0,0,1,0,0,0,0,0,0},
    {0,0,0,0,1,1,1,0,0,0,0,0},
    {0,0,0,1,0,1,0,1,0,0,0,0},
    {0,0,0,0,0,1,0,0,0,0,0,0},
    {0,0,0,0,0,1,0,0,0,0,0,0},
    {0,0,0,1,0,1,0,1,0,0,0,0},
    {0,0,0,0,1,1,1,0,0,0,0,0},
    {0,0,0,0,0,1,0,0,0,0,0,0},
  };

  uint8_t frame[8][12];
  memset(frame, 0, sizeof(frame));

  if(matrix_pagina == 0) {
    int leds = min((int)ctrl.stand, 12);
    for(int col = 0; col < leds; col++)
      for(int row = 0; row < 8; row++) frame[row][col] = 1;
  } else if(matrix_pagina == 1) {
    const uint8_t (*icon)[12];
    if     (defrost)                          icon = ICON_DEFROST;
    else if(koeling_modus && ctrl.wp_aan)     icon = ICON_SNOW;
    else if(ctrl.wp_aan)                      icon = ICON_FLAME;
    else                                      icon = ICON_OFF;
    memcpy(frame, icon, sizeof(frame));
  } else {
    const uint8_t (*icon)[12];
    if     (modus == Modus::FF_AUTO)   icon = MODUS_FF_AUTO;
    else if(modus == Modus::WATER)     icon = MODUS_WATER;
    else if(modus == Modus::FF_WATER)  icon = MODUS_FF_WATER;
    else if(modus == Modus::HANDMATIG) icon = MODUS_HAND;
    else                               icon = MODUS_AUTO;
    memcpy(frame, icon, sizeof(frame));
  }

  matrix_pagina = (matrix_pagina + 1) % 3;
#if defined(ARDUINO_UNOR4_WIFI)
  matrix.renderBitmap(frame, 8, 12);
#endif
}

// ═══════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════

void setup(){
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n\nKromhout WP v2.0 — FF modus");
#if defined(ARDUINO_UNOR4_WIFI)
  if(USE_LED_MATRIX) matrix.begin();
#endif
  EEPROM_BEGIN();
  eeprom_init();
#if defined(ARDUINO_UNOR4_WIFI)
  chofuSerial.begin(9600);
#else
  chofuSerial.begin(9600, SERIAL_8N1, CHOFU_RX_PIN, CHOFU_TX_PIN);
#endif

  if(USE_LCD){
    lcd.init(); lcd.init(); lcd.backlight();
    lcd.print("Kromhout WP"); lcd.setCursor(0,1); lcd.print("v2.0 FF");
    delay(2000);
  }

  lcd.clear(); lcd.print("WiFi...");
  WiFi.begin(SSID, PASS);
  int attempts = 0;
  while(WiFi.status() != WL_CONNECTED && attempts < 20){ Serial.print("."); delay(500); attempts++; }
  if(WiFi.status() == WL_CONNECTED){
    while(WiFi.localIP() == IPAddress(0,0,0,0) && attempts < 30){ delay(1000); attempts++; }
    Serial.print("WiFi OK! IP: "); Serial.println(WiFi.localIP());
    lcd.clear(); lcd.print("WiFi OK!"); lcd.setCursor(0,1); lcd.print(WiFi.localIP());
    delay(2000);
  }

  webServer.begin();
  lcd.clear(); lcd.print("MQTT...");
  mqttClient.setUsernamePassword(MQTT_USER, MQTT_PASS);
  mqttClient.onMessage(mqtt_ontvang);
  mqttClient.beginWill("chofu/status", true, 1);
  mqttClient.print("offline");
  mqttClient.endWill();

  if(mqttClient.connect(MQTT_BROKER, MQTT_PORT)){
    Serial.println("MQTT OK!");
    mqttClient.subscribe("chofu/cmd/#");
    mqttClient.subscribe("chofu/sim/#");
    mqttClient.subscribe("anna/setpoint");
    mqttClient.subscribe("anna/temperatuur");
    mqttClient.beginMessage("chofu/status", true, 1);
    mqttClient.print("online");
    mqttClient.endMessage();
    delay(1000);
    lcd.clear(); lcd.print("Discovery...");
    discovery_fase1();
    vorige_discovery_ms = millis();
    discovery_fase = 1;
  }

  Serial.println("Systeem operationeel");
  Serial.print("Web: http://"); Serial.println(WiFi.localIP());
}

// ═══════════════════════════════════════════════════════════════
//  MAIN LOOP
// ═══════════════════════════════════════════════════════════════

void mqtt_herverbind(){
  if(mqttClient.connected()) return;
  Serial.println("MQTT: herverbinden...");
  mqttClient.beginWill("chofu/status", true, 1);
  mqttClient.print("offline");
  mqttClient.endWill();
  if(mqttClient.connect(MQTT_BROKER, MQTT_PORT)){
    Serial.println("MQTT: herverbonden");
    mqttClient.subscribe("chofu/cmd/#");
    mqttClient.subscribe("chofu/sim/#");
    mqttClient.subscribe("anna/setpoint");
    mqttClient.subscribe("anna/temperatuur");
    mqttClient.beginMessage("chofu/status", true, 1);
    mqttClient.print("online");
    mqttClient.endMessage();
    discovery_fase = 0; discovery_fase1();
    vorige_discovery_ms = millis(); discovery_fase = 1;
  } else {
    Serial.println("MQTT: herverbinden mislukt"); delay(5000);
  }
}

void loop(){
  mqtt_herverbind();
  mqttClient.poll();
  check_mqtt_watchdog();
  lees_warmtepomp_data();
  pas_sim_toe();
  pas_pid_aan();
  update_lcd();
  update_matrix();
  if(millis() - vorige_data_ms > 10000) stuur_data();
  if(discovery_fase == 1 && millis() - vorige_discovery_ms > 30000){
    discovery_fase2(); vorige_discovery_ms = millis(); discovery_fase = 2;
  }
  if(discovery_fase == 2 && millis() - vorige_discovery_ms > 30000){
    discovery_fase3(); discovery_fase = 3;
  }
  handle_web_client();
  delay(10);
}
