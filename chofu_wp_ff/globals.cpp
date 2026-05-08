#include "globals.h"

// ═══════════════════════════════════════════════════════════════
//  HARDWARE OBJECTEN
// ═══════════════════════════════════════════════════════════════

HardwareSerial&   chofuSerial = Serial1;
WiFiClient        wifiClient;
MqttClient        mqttClient(wifiClient);
LiquidCrystal_I2C lcd(0x27, 16, 2);
#if defined(ARDUINO_UNOR4_WIFI)
ArduinoLEDMatrix  matrix;
#endif
WiFiServer webServer(80);

// ═══════════════════════════════════════════════════════════════
//  WARMTEPOMP DATA
// ═══════════════════════════════════════════════════════════════

float   t_supply = 25.0, t_return = 20.0, t_outside = 5.0;
uint8_t comp_hz = 0;
uint8_t pomp_snelheid_wp = 0;
bool    defrost = false;
float   werkelijk_vermogen_w = 0;

// ═══════════════════════════════════════════════════════════════
//  KAMER / WATER
// ═══════════════════════════════════════════════════════════════

float t_kamer = 19.0;
float t_kamer_gewenst = 19.0;
float t_water_gewenst = 32.0;
bool  koeling_modus = false;

// ═══════════════════════════════════════════════════════════════
//  REGELPARAMETERS
// ═══════════════════════════════════════════════════════════════

float   setpoint = 28.0;
float   doel_setpoint = 40.0;
float   delta_t = 5.0;
bool    lcd_enabled = true;
Modus   modus = Modus::AUTO;
uint8_t handmatig_stand = 1;

float Kp = 19.9;
float Ki = 0.084;
float Kd = 0.036;

long HYST_SLOW_MS = 600000;  // 10 minuten
long HYST_FAST_MS = 120000;  //  2 minuten
long HYST_DOWN_MS =  60000;  //  1 minuut
long pid_interval_ms = 5000; //  5 seconden

float STOOKLIJN_GRENS  = 15.0;
float STOOKLIJN_FACTOR = 0.68;
float T_VORST          = 2.0;

float SUPPLY_MAX           = 60.0;
float KOELING_MIN_BUITEN   = 18.0;
float STOOKLIJN_UIT_GRENS  = 15.0;
long  MQTT_WATCHDOG_MS     = 7200000;

float ff_UA_house   = 272.5;  // [W/K] lerende UA huis  — geoptimaliseerd KGE
float ff_UA_emitter = 267.5;  // [W/K] lerende UA emitter — geoptimaliseerd KGE

// ═══════════════════════════════════════════════════════════════
//  CONTROLLER TOESTAND
// ═══════════════════════════════════════════════════════════════

ControllerState ctrl;

// ═══════════════════════════════════════════════════════════════
//  PROTOCOL BUFFERS
// ═══════════════════════════════════════════════════════════════

uint8_t telegram_buffer[25];
uint8_t buffer_index = 0;

float prev_t_supply  = 25.0;
float prev_t_return  = 20.0;
float prev_t_outside =  5.0;

// ═══════════════════════════════════════════════════════════════
//  SIMULATIE
// ═══════════════════════════════════════════════════════════════

float sim_t_supply        = NAN;
float sim_t_return        = NAN;
float sim_t_outside       = NAN;
float sim_t_water_gewenst = NAN;
float sim_t_kamer         = NAN;
float sim_t_kamer_gewenst = NAN;

// ═══════════════════════════════════════════════════════════════
//  TIMERS
// ═══════════════════════════════════════════════════════════════

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
//  MQTT LOGGING STATE
// ═══════════════════════════════════════════════════════════════

bool     mqtt_logging_enabled = true;
uint32_t laatste_log_ms = 0;
bool     data_sturen_gevraagd = false;
