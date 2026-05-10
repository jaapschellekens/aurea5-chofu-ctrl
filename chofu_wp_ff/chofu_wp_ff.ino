
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
  Serial.println("\n\nKromhout WP v2.0 — FF modus");
#if defined(ARDUINO_UNOR4_WIFI)
  if(USE_LED_MATRIX) matrix.begin();
#endif
  EEPROM_BEGIN();
  eeprom_init();
  knoppen_init();
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
  check_knoppen();
  check_mqtt_watchdog();
  lees_warmtepomp_data();
  pas_sim_toe();
  pas_pid_aan();
  update_lcd();
  update_matrix();
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
