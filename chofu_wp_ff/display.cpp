#include "display.h"

// ═══════════════════════════════════════════════════════════════
//  LCD
// ═══════════════════════════════════════════════════════════════

void update_lcd(){
  if(!USE_LCD || !lcd_enabled) return;
  uint32_t nu = millis();
  if(nu - vorige_lcd_ms < 6000) return;
  vorige_lcd_ms = nu;

  static uint8_t scherm = 0;
  int verm = (werkelijk_vermogen_w > 0) ? (int)werkelijk_vermogen_w : VERMOGEN[ctrl.stand];
  uint8_t max_scherm = proto_logging ? 5 : 4;

  lcd.clear();

  switch(scherm){

    // ── Scherm 0: Altijd gelijk — statussamenvatting ─────────────
    case 0:
      // r0: "St3 1200W  AAN"
      lcd.print("St"); lcd.print(ctrl.stand);
      lcd.print(" "); lcd.print(verm); lcd.print("W");
      lcd.print(ctrl.wp_aan ? " AAN" : " UIT");
      // r1: "FF-A  Hz:45"
      lcd.setCursor(0,1);
      if     (modus==Modus::FF_AUTO)   lcd.print("FF-A");
      else if(modus==Modus::FF_WATER)  lcd.print("FF-W");
      else if(modus==Modus::WATER)     lcd.print("WATR");
      else if(modus==Modus::HANDMATIG) lcd.print("HAND");
      else                             lcd.print("AUTO");
      lcd.print(" Hz:"); lcd.print(comp_hz);
      break;

    // ── Scherm 1: Primaire regelstatus (mode-specifiek) ──────────
    case 1:
      switch(modus){

        case Modus::AUTO:
          // r0: "K:20.1 >21.0"    kamer actual vs setpoint
          // r1: "PID:45%  F:-0.9" PID output + regelafwijking kamer
          lcd.print("K:"); lcd.print(t_kamer,1);
          lcd.print(" >"); lcd.print(t_kamer_gewenst,1);
          lcd.setCursor(0,1);
          lcd.print("PID:"); lcd.print((int)ctrl.pid_output); lcd.print("%");
          { float f = t_kamer_gewenst - t_kamer;
            lcd.print(" F:"); if(f>0) lcd.print("+"); lcd.print(f,1); }
          break;

        case Modus::FF_AUTO:
          // r0: "K:20.1 >21.0"    kamer actual vs setpoint
          // r1: "B: 5.0  UA:272"  buiten + geleerd UA_house
          lcd.print("K:"); lcd.print(t_kamer,1);
          lcd.print(" >"); lcd.print(t_kamer_gewenst,1);
          lcd.setCursor(0,1);
          lcd.print("B:"); lcd.print(t_outside,1);
          lcd.print(" UA:"); lcd.print((int)ff_UA_house);
          break;

        case Modus::WATER:
          // r0: "A:35.2 SP:32"    aanvoer vs water setpoint
          // r1: "PID:45%  F:+3.2" PID output + regelafwijking aanvoer
          lcd.print("A:"); lcd.print(t_supply,1);
          lcd.print(" SP:"); lcd.print(t_water_gewenst,0);
          lcd.setCursor(0,1);
          lcd.print("PID:"); lcd.print((int)ctrl.pid_output); lcd.print("%");
          { float f = t_supply - t_water_gewenst;
            lcd.print(" F:"); if(f>0) lcd.print("+"); lcd.print(f,1); }
          break;

        case Modus::FF_WATER:
          // r0: "A:35.2 D:38.2"   aanvoer vs stooklijn doel
          // r1: "B: 5.0  UA:267"  buiten + geleerd UA_emitter
          lcd.print("A:"); lcd.print(t_supply,1);
          lcd.print(" D:"); lcd.print(doel_setpoint,1);
          lcd.setCursor(0,1);
          lcd.print("B:"); lcd.print(t_outside,1);
          lcd.print(" UA:"); lcd.print((int)ff_UA_emitter);
          break;

        default: // HANDMATIG
          // r0: "A:35.2  R:30.1"  aanvoer + retour
          // r1: "DT: 5.1  Hz:45"  delta_T + compressorfrequentie
          lcd.print("A:"); lcd.print(t_supply,1);
          lcd.print(" R:"); lcd.print(t_return,1);
          lcd.setCursor(0,1);
          lcd.print("DT:"); lcd.print(delta_t,1);
          lcd.print(" Hz:"); lcd.print(comp_hz);
          break;
      }
      break;

    // ── Scherm 2: Secundaire info (mode-specifiek) ───────────────
    case 2:
      switch(modus){

        case Modus::AUTO:
          // r0: "A:35.2 D:38.2"   aanvoer vs stooklijn doel
          // r1: "B: 5.0  R:30.1"  buiten + retour
          lcd.print("A:"); lcd.print(t_supply,1);
          lcd.print(" D:"); lcd.print(doel_setpoint,1);
          lcd.setCursor(0,1);
          lcd.print("B:"); lcd.print(t_outside,1);
          lcd.print(" R:"); lcd.print(t_return,1);
          break;

        case Modus::FF_AUTO:
          // r0: "A:35.2  R:30.1"  aanvoer + retour
          // r1: "DT:5.1 FF:+0.5"  delta_T + FF integraalcorrectie
          lcd.print("A:"); lcd.print(t_supply,1);
          lcd.print(" R:"); lcd.print(t_return,1);
          lcd.setCursor(0,1);
          lcd.print("DT:"); lcd.print(delta_t,1);
          lcd.print(" FF:");
          if(ctrl.ff_integraal >= 0) lcd.print("+");
          lcd.print(ctrl.ff_integraal,1);
          break;

        case Modus::WATER:
          // r0: "R:30.1 DT: 5.1"  retour + delta_T
          // r1: "B: 5.0  K:20.1"  buiten + kamer
          lcd.print("R:"); lcd.print(t_return,1);
          lcd.print(" DT:"); lcd.print(delta_t,1);
          lcd.setCursor(0,1);
          lcd.print("B:"); lcd.print(t_outside,1);
          lcd.print(" K:"); lcd.print(t_kamer,1);
          break;

        case Modus::FF_WATER:
          // r0: "R:30.1 DT: 5.1"  retour + delta_T
          // r1: "FF:+0.5  Hz:45"  FF integraalcorrectie + Hz
          lcd.print("R:"); lcd.print(t_return,1);
          lcd.print(" DT:"); lcd.print(delta_t,1);
          lcd.setCursor(0,1);
          lcd.print("FF:");
          if(ctrl.ff_integraal >= 0) lcd.print("+");
          lcd.print(ctrl.ff_integraal,1);
          lcd.print(" Hz:"); lcd.print(comp_hz);
          break;

        default: // HANDMATIG
          // r0: "K:20.1  B: 5.0"  kamer + buiten
          // r1: "Verm: 1200W"      huidig vermogen
          lcd.print("K:"); lcd.print(t_kamer,1);
          lcd.print(" B:"); lcd.print(t_outside,1);
          lcd.setCursor(0,1);
          lcd.print("Verm:"); lcd.print(verm); lcd.print("W");
          break;
      }
      break;

    // ── Scherm 3: Altijd gelijk — netwerk + pomp ─────────────────
    case 3:
      // r0: "P:60%  DT: 5.1"
      // r1: "192.168.1.50"
      lcd.print("P:"); lcd.print(pomp_snelheid_wp); lcd.print("%");
      lcd.print(" DT:"); lcd.print(delta_t,1);
      lcd.setCursor(0,1);
      lcd.print(WiFi.localIP());
      break;
    // ── Scherm 4: Protocol debug (alleen als proto_logging aan) ──
    case 4: {
      // r0: "DBG klas  OK" of "DBG jgc TOUT"
      // r1: "A:35.2 Err:0"
      bool timeout = (millis() - vorige_telegram_ms > 5000);
      lcd.print("DBG ");
      lcd.print(parser_jgc ? "jgc " : "klas");
      lcd.print(timeout ? " TOUT" : " OK  ");
      lcd.setCursor(0, 1);
      lcd.print("A:"); lcd.print(t_supply, 1);
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
  uint32_t fb[3] = {0, 0, 0};
  for(int row = 0; row < 8; row++){
    for(int col = 0; col < 12; col++){
      if(frame[row][col]){
        int idx = row * 12 + col;
        fb[idx / 32] |= (1u << (31 - (idx % 32)));
      }
    }
  }
  Serial.print("matrix upd: pagina="); Serial.print((matrix_pagina+2)%3);
  Serial.print(" fb=0x"); Serial.print(fb[0],HEX);
  Serial.print(",0x"); Serial.print(fb[1],HEX);
  Serial.print(",0x"); Serial.println(fb[2],HEX);
  // DEBUG: laad hart vanuit display.cpp om te testen of loadFrame hier werkt
  matrix.loadFrame(LEDMATRIX_HEART_BIG);
#endif
}
