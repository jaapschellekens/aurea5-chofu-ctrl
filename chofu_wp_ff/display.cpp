#include "display.h"

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
