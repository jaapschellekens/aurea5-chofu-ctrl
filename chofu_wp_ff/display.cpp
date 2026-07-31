#include "display.h"
#include <Wire.h>
#include "mqtt.h"   // mqtt_log bij LCD-herstel

// ═══════════════════════════════════════════════════════════════
//  LCD
// ═══════════════════════════════════════════════════════════════

// True als de LCD-backpack op de I²C-bus ACK't (0x27).
static bool lcd_bus_ok(){
  Wire.beginTransmission(0x27);
  return (Wire.endTransmission() == 0);
}

// Maak een vastgelopen I²C-bus weer vlot: een slave die SDA laag houdt (door een
// glitch/EMI) wordt losgemaakt door SCL 9× handmatig te klokken, gevolgd door een
// STOP-conditie. Daarna Wire + LCD opnieuw initialiseren.
static void lcd_bus_herstel(){
  Wire.end();
  pinMode(SCL, OUTPUT);
  pinMode(SDA, INPUT_PULLUP);
  for(uint8_t i = 0; i < 9; i++){
    digitalWrite(SCL, HIGH); delayMicroseconds(5);
    digitalWrite(SCL, LOW);  delayMicroseconds(5);
  }
  // STOP: SDA laag→hoog terwijl SCL hoog
  pinMode(SDA, OUTPUT);
  digitalWrite(SDA, LOW);  delayMicroseconds(5);
  digitalWrite(SCL, HIGH); delayMicroseconds(5);
  digitalWrite(SDA, HIGH); delayMicroseconds(5);
  Wire.begin();
  lcd.init();
  lcd.backlight();
}

void update_lcd(){
  if(!USE_LCD || !lcd_enabled) return;
  uint32_t nu = millis();
  if(nu - vorige_lcd_ms < 6000) return;
  vorige_lcd_ms = nu;

  // Eenmalige detectie: is er überhaupt een LCD aangesloten? Zo niet (bv. testbord)
  // → LCD-functies uit, anders blijft de bus-check elke 6s vals "herstellen".
  static int8_t lcd_status = -1;   // -1=onbekend, 0=afwezig, 1=aanwezig
  if(lcd_status == -1){
    lcd_status = lcd_bus_ok() ? 1 : 0;
    if(lcd_status == 0){ mqtt_log("Geen LCD op I2C gevonden — LCD uitgeschakeld", "INFO"); return; }
  }
  if(lcd_status == 0) return;

  // I²C-bewaking: een vastgelopen bus bevriest het LCD terwijl de rest van de
  // firmware doordraait. Detecteer dat en herstel de bus + her-init het display.
  if(!lcd_bus_ok()){
    lcd_bus_herstel();
    mqtt_log("LCD I2C vastgelopen — bus hersteld", "WARNING");
  }

  // Periodieke her-init: een HD44780-desync (voedingsglitch e.d.) bevriest het
  // beeld terwijl de PCF8574 nog ACK't, dus niet via de bus-check te vangen.
  // Elke 5 min opnieuw initialiseren begrenst een eventuele bevriezing tot ~5 min.
  static uint32_t laatste_init_ms = 0;
  if(nu - laatste_init_ms > 300000UL){
    laatste_init_ms = nu;
    lcd.init();
    lcd.backlight();
  }

  // ── Vereenvoudigde weergave: 3 schermen (+ debug bij proto_log) ──
  // Scherm 0 = wat doet hij nu, Scherm 1 = omgeving, Scherm 2 = systeem.
  static uint8_t scherm = 0;
  uint8_t max_scherm = proto_logging ? 4 : 3;

  lcd.clear();

  switch(scherm){

    // ── Scherm 0: Wat doet hij nu ────────────────────────────────
    case 0: {
      // Regel 1: toestandwoord + stand.
      const char* toestand; bool toon_stand = true;
      if(defrost)             { toestand = "ONTDOOIEN"; toon_stand = false; }
      else if(sww_actief)       toestand = "TAPWATER";
      else if(!ctrl.wp_aan)   { toestand = "UIT";       toon_stand = false; }
      else if(koeling_modus)    toestand = "KOELEN";
      else                      toestand = "VERWARMEN";
      lcd.print(toestand);
      if(toon_stand){ lcd.setCursor(11,0); lcd.print("st "); lcd.print(ctrl.stand); }

      // Regel 2: geregelde temperatuur passend bij de situatie/modus.
      lcd.setCursor(0,1);
      if(sww_actief){
        lcd.print("Tapw "); lcd.print(t_supply,1); lcd.print(">"); lcd.print(SWW_SETPOINT,1);
      } else if(!ctrl.wp_aan){
        lcd.print("Buiten "); lcd.print(t_outside,1); lcd.print(" C");
      } else if(modus==Modus::AUTO || modus==Modus::FF_AUTO){
        lcd.print("Kamer "); lcd.print(t_kamer,1); lcd.print(">"); lcd.print(t_kamer_gewenst,1);
      } else if(modus==Modus::WATER || modus==Modus::FF_WATER){
        lcd.print("Aanv "); lcd.print(t_supply,1); lcd.print(">"); lcd.print(t_water_gewenst,1);
      } else { // HANDMATIG
        lcd.print("Aanv "); lcd.print(t_supply,0); lcd.print(" Ret "); lcd.print(t_return,0);
      }
      break;
    }

    // ── Scherm 1: Omgeving & waterkring ──────────────────────────
    case 1:
      lcd.print("Buiten "); lcd.print(t_outside,1); lcd.print(" W"); lcd.print((int)werkelijk_vermogen_w);
      lcd.setCursor(0,1);
      lcd.print("Aanv "); lcd.print(t_supply,0); lcd.print(" Ret "); lcd.print(t_return,0);
      break;

    // ── Scherm 2: Systeem ────────────────────────────────────────
    case 2:
      lcd.print(WiFi.localIP());
      lcd.setCursor(0,1);
      lcd.print("Modus: "); lcd.print(modus_naar_str(modus));
      break;

    // ── Scherm 3: Protocol-debug (alleen als proto_logging aan) ──
    case 3: {
      bool timeout = (millis() - vorige_telegram_ms > 5000);
      lcd.print("DBG jgc ");
      lcd.print(timeout ? "TOUT" : "OK  ");
      lcd.setCursor(0, 1);
      lcd.print("Aanv "); lcd.print(t_supply, 1);
      lcd.print(" Err:"); lcd.print(proto_crc_fouten);
      break;
    }
  }

  scherm = (scherm + 1) % max_scherm;
}

// ═══════════════════════════════════════════════════════════════
//  LED MATRIX
// ═══════════════════════════════════════════════════════════════

void update_matrix() {
  if(!USE_LED_MATRIX) return;
  uint32_t nu = millis();
  if(nu - vorige_matrix_ms < 2000) return;
  vorige_matrix_ms = nu;

  // Pagina 0 — stand bar: elke kolom = één stand (0–10, matrix is 12 breed)
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

  // Pagina 2 — modus icoon
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
    int leds = min((int)ctrl.stand, 10);
    if(leds == 0) {
      // WP uit: één pixel in het midden als standby-indicator
      frame[3][5] = 1; frame[3][6] = 1;
      frame[4][5] = 1; frame[4][6] = 1;
    } else {
      for(int col = 0; col < leds; col++)
        for(int row = 0; row < 8; row++) frame[row][col] = 1;
    }
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
  // Bereken uint32_t[3] frame: LED-index i → bit (31-i%32) van fb[i/32].
  // loadFrame() wordt door de .ino aangeroepen (zelfde TU als ISR/framebuffer).
  matrix_fb[0] = matrix_fb[1] = matrix_fb[2] = 0;
  for(int row = 0; row < 8; row++){
    for(int col = 0; col < 12; col++){
      if(frame[row][col]){
        int idx = row * 12 + col;
        matrix_fb[idx / 32] |= (1u << (31 - (idx % 32)));
      }
    }
  }
  matrix_fb_klaar = true;
#endif
}
