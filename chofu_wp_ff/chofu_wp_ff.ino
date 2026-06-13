
/*
 * ╔═══════════════════════════════════════════════════════════════╗
 * ║  ChofuCtrl v2.0 — FF modus                                  ║
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
 *   "ff_water" — Feedforward op aanvoertemperatuur (extern setpoint Adam)
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

// WiFi & MQTT — credentials staan in config.h (niet in git)
#include "config.h"

// Typen, structs en constanten
#include "types.h"

// Globale variabelen (extern declaraties + library-includes)
#include "globals.h"

// Modules
#include "eeprom.h"
#include "protocol.h"
#include "regelaar.h"
#include "mqtt.h"
#include "display.h"
#include "web.h"
#include "knoppen.h"

// ═══════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════

void setup(){
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n\nChofuCtrl v2.0 — FF modus");
  EEPROM_BEGIN();
  eeprom_init();
  knoppen_init();
#if defined(ARDUINO_UNOR4_WIFI)
  chofuSerial.begin(666);
#else
  chofuSerial.begin(666, SERIAL_8N1, CHOFU_RX_PIN, CHOFU_TX_PIN);
#endif

  if(USE_LCD){
    lcd.init(); lcd.init(); lcd.backlight();
    lcd.print("ChofuCtrl"); lcd.setCursor(0,1); lcd.print("v2.0 FF");
    delay(2000);
  }

  #if USE_LCD
  lcd.clear(); lcd.print("WiFi...");
  #endif
  WiFi.begin(SSID, PASS);
  int attempts = 0;
  while(WiFi.status() != WL_CONNECTED && attempts < 20){ Serial.print("."); delay(500); attempts++; }
  if(WiFi.status() == WL_CONNECTED){
    while(WiFi.localIP() == IPAddress(0,0,0,0) && attempts < 30){ delay(1000); attempts++; }
    Serial.print("WiFi OK! IP: "); Serial.println(WiFi.localIP());
    #if USE_LCD
    lcd.clear(); lcd.print("WiFi OK!"); lcd.setCursor(0,1); lcd.print(WiFi.localIP());
    delay(2000);
    #endif
  }

  webServer.begin();
  #if USE_LCD
  lcd.clear(); lcd.print("MQTT...");
  #endif
  Serial.print("MQTT: verbinden met "); Serial.print(MQTT_BROKER);
  Serial.print(":"); Serial.println(MQTT_PORT);
  Serial.print("MQTT: gebruiker="); Serial.println(MQTT_USER);
  mqttClient.setUsernamePassword(MQTT_USER, MQTT_PASS);
  mqttClient.onMessage(mqtt_ontvang);
  Serial.println("MQTT: LWT instellen (chofu/status = offline)");
  mqttClient.beginWill("chofu/status", true, 1);
  mqttClient.print("offline");
  mqttClient.endWill();

  Serial.println("MQTT: connect()...");
  if(mqttClient.connect(MQTT_BROKER, MQTT_PORT)){
    Serial.println("MQTT OK!");
    Serial.println("MQTT: abonneren op chofu/cmd/#, chofu/sim/#");
    mqttClient.subscribe("chofu/cmd/#");
    mqttClient.subscribe("chofu/sim/#");
    Serial.println("MQTT: publiceren chofu/status = online");
    mqttClient.beginMessage("chofu/status", true, 1);
    mqttClient.print("online");
    mqttClient.endMessage();
    delay(1000);
    #if USE_LCD
    lcd.clear(); lcd.print("Discovery...");
    #endif
    discovery_fase1();
    vorige_discovery_ms = millis();
    discovery_fase = 1;
  } else {
    Serial.print("MQTT: verbinding mislukt, foutcode="); Serial.println(mqttClient.connectError());
  }

  Serial.println("Systeem operationeel");
  Serial.print("Web: http://"); Serial.println(WiFi.localIP());
#if defined(ARDUINO_UNOR4_WIFI)
  // matrix.begin() MOET na WiFi.begin() komen: de WiFi-library reset op de UNO R4
  // bepaalde hardware-timers waardoor de ISR van de LED-matrix stopt.
  if(USE_LED_MATRIX){
    bool ok = matrix.begin();
    Serial.print("LED matrix begin(): "); Serial.println(ok ? "OK" : "MISLUKT");
  }
#endif
  boot_ms = millis();
  Serial.println("Koude-start vertraging: 5 min stand=0");
}

// ═══════════════════════════════════════════════════════════════
//  MAIN LOOP
// ═══════════════════════════════════════════════════════════════

void mqtt_herverbind(){
  // WiFi eerst: MQTT kan niet werken zonder WiFi
  if(WiFi.status() != WL_CONNECTED){
    Serial.println("WiFi: herverbinden...");
    WiFi.disconnect();
    WiFi.begin(SSID, PASS);
    uint32_t t = millis();
    while(WiFi.status() != WL_CONNECTED && millis() - t < 10000) delay(500);
    if(WiFi.status() != WL_CONNECTED){
      Serial.println("WiFi: herverbinden mislukt");
      return;
    }
    Serial.print("WiFi: herverbonden, IP: "); Serial.println(WiFi.localIP());
  }

  if(mqttClient.connected()) return;
  Serial.print("MQTT: herverbinden met "); Serial.print(MQTT_BROKER);
  Serial.print(":"); Serial.println(MQTT_PORT);
  mqttClient.beginWill("chofu/status", true, 1);
  mqttClient.print("offline");
  mqttClient.endWill();
  if(mqttClient.connect(MQTT_BROKER, MQTT_PORT)){
    Serial.println("MQTT: herverbonden");
    mqttClient.subscribe("chofu/cmd/#");
    mqttClient.subscribe("chofu/sim/#");
    mqttClient.beginMessage("chofu/status", true, 1);
    mqttClient.print("online");
    mqttClient.endMessage();
    discovery_fase = 0; discovery_fase1();
    vorige_discovery_ms = millis(); discovery_fase = 1;
  } else {
    Serial.print("MQTT: herverbinden mislukt, foutcode="); Serial.println(mqttClient.connectError());
    delay(5000);
  }
}

void loop(){
  mqtt_herverbind();
  mqttClient.poll();
  check_knoppen();
  check_mqtt_watchdog();

  // Koude-start vertraging: eerste 5 min na boot altijd stand=0
  static bool boot_delay_afgelopen = false;
  if(!boot_delay_afgelopen){
    if(millis() - boot_ms < BOOT_DELAY_MS){
      ctrl.stand = 0;
      ctrl.wp_aan = false;
    } else {
      boot_delay_afgelopen = true;
      mqtt_log("Koude-start vertraging afgelopen — regelaar actief", "INFO");
    }
  }

  lees_warmtepomp_data();
  pas_sim_toe();
  pas_pid_aan();
  update_lcd();
  update_matrix();
#if defined(ARDUINO_UNOR4_WIFI)
  // loadFrame() hier aanroepen (zelfde TU als ISR) zodat de juiste 'framebuffer'
  // static variabele wordt bijgewerkt. display.cpp heeft een eigen kopie.
  if(USE_LED_MATRIX && matrix_fb_klaar){
    matrix_fb_klaar = false;
    matrix.loadFrame(matrix_fb);
  }
#endif
  if(data_sturen_gevraagd || millis() - vorige_data_ms > 10000){
    data_sturen_gevraagd = false;
    stuur_data();
  }
  if(discovery_fase == 1 && millis() - vorige_discovery_ms > 30000){
    discovery_fase2(); vorige_discovery_ms = millis(); discovery_fase = 2;
  }
  if(discovery_fase == 2 && millis() - vorige_discovery_ms > 30000){
    discovery_fase3(); discovery_fase = 3;
  }
  handle_web_client();
  delay(10);
}
