#pragma once

#if defined(ARDUINO_UNOR4_WIFI)
  #include <WiFiS3.h>
  #include <Arduino_LED_Matrix.h>
#else
  #include <WiFi.h>
#endif
#include <ArduinoMqttClient.h>
#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>

#include "types.h"

// ═══════════════════════════════════════════════════════════════
//  HARDWARE OBJECTEN
// ═══════════════════════════════════════════════════════════════

extern HardwareSerial&   chofuSerial;
extern WiFiClient        wifiClient;
extern MqttClient        mqttClient;
extern LiquidCrystal_I2C lcd;
#if defined(ARDUINO_UNOR4_WIFI)
extern ArduinoLEDMatrix  matrix;
#endif
extern WiFiServer webServer;

// ═══════════════════════════════════════════════════════════════
//  WARMTEPOMP DATA
// ═══════════════════════════════════════════════════════════════

extern float   t_supply, t_return, t_outside;
extern uint8_t comp_hz;
extern uint8_t pomp_snelheid_wp;
extern bool    defrost;
extern float   werkelijk_vermogen_w;

// ═══════════════════════════════════════════════════════════════
//  KAMER / WATER
// ═══════════════════════════════════════════════════════════════

extern float t_kamer;
extern float t_kamer_gewenst;
extern float t_water_gewenst;
extern bool  koeling_modus;

// ═══════════════════════════════════════════════════════════════
//  REGELPARAMETERS (instelbaar via MQTT, opgeslagen in EEPROM)
// ═══════════════════════════════════════════════════════════════

extern float   setpoint;
extern float   doel_setpoint;
extern float   delta_t;
extern bool    lcd_enabled;
extern Modus   modus;
extern uint8_t handmatig_stand;

// PID
extern float Kp, Ki, Kd;

// Hysteresis tijden
extern long HYST_SLOW_MS;
extern long HYST_FAST_MS;
extern long HYST_DOWN_MS;
extern long pid_interval_ms;

// Stooklijn
extern float STOOKLIJN_GRENS;
extern float STOOKLIJN_FACTOR;
extern float T_VORST;

// Safeguards
extern float SUPPLY_MAX;
extern float KOELING_MIN_BUITEN;
extern float STOOKLIJN_UIT_GRENS;
extern long  MQTT_WATCHDOG_MS;

// FF leerwaarden (instelbaar via MQTT, opgeslagen in EEPROM)
extern float ff_UA_house;
extern float ff_UA_emitter;

// ═══════════════════════════════════════════════════════════════
//  CONTROLLER TOESTAND
// ═══════════════════════════════════════════════════════════════

extern ControllerState ctrl;

// ═══════════════════════════════════════════════════════════════
//  PROTOCOL BUFFERS
// ═══════════════════════════════════════════════════════════════

extern uint8_t telegram_buffer[25];
extern uint8_t buffer_index;

// Spike filter (vorige waarden voor plausibiliteitscheck)
extern float prev_t_supply;
extern float prev_t_return;
extern float prev_t_outside;

// ═══════════════════════════════════════════════════════════════
//  SIMULATIE
// ═══════════════════════════════════════════════════════════════

extern float sim_t_supply;
extern float sim_t_return;
extern float sim_t_outside;
extern float sim_t_water_gewenst;
extern float sim_t_kamer;
extern float sim_t_kamer_gewenst;

inline bool sim_actief(){
  return !isnan(sim_t_supply) || !isnan(sim_t_return) || !isnan(sim_t_outside)
      || !isnan(sim_t_water_gewenst) || !isnan(sim_t_kamer);
}

// ═══════════════════════════════════════════════════════════════
//  TIMERS
// ═══════════════════════════════════════════════════════════════

extern uint8_t  discovery_fase;
extern uint32_t vorige_discovery_ms;
extern uint32_t vorige_data_ms;
extern uint32_t vorige_lcd_ms;
extern uint32_t vorige_matrix_ms;
extern uint8_t  matrix_pagina;
extern uint32_t vorige_pid_ms;
extern uint32_t vorige_telegram_ms;
extern uint32_t vorige_web_check_ms;
extern uint32_t vorige_mqtt_rx_ms;

// ═══════════════════════════════════════════════════════════════
//  MQTT LOGGING
// ═══════════════════════════════════════════════════════════════

extern bool     mqtt_logging_enabled;
extern uint32_t laatste_log_ms;

// Vlag: stuur_data() is aangevraagd vanuit de MQTT-callback maar wordt
// afgehandeld in de main loop om re-entrant gebruik van mqttClient te voorkomen.
extern bool     data_sturen_gevraagd;

constexpr uint32_t LOG_THROTTLE_MS = 500;
