#include "regelaar.h"
#include "mqtt.h"    // voor mqtt_log() en stuur_alert()
#include "eeprom.h"  // (niet direct nodig, maar eeprom_save via mqtt_ontvang; hier niet gebruikt)

// ─── Hulpfuncties (bestandslocaal) ───────────────────────────────

// COP schatting op basis van aanvoer- en buitentemperatuur (Carnot × eta=0.40)
static float ff_cop(float T_aanvoer, float T_buiten){
  float T_s = T_aanvoer + 273.15f;
  float T_o = T_buiten  + 273.15f;
  float dT  = T_s - T_o;
  if(dT < 1.0f) return 1.0f;
  return constrain(T_s / dT * 0.40f, 1.0f, 6.0f);
}

// COP koeling: warmte onttrokken aan koud water / elektrisch vermogen
static float ff_cop_koel(float T_aanvoer, float T_buiten){
  float T_s = T_aanvoer + 273.15f;
  float T_o = T_buiten  + 273.15f;
  float dT  = T_o - T_s;  // buiten warmer dan koude aanvoer
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

// ═══════════════════════════════════════════════════════════════
//  FEEDFORWARD KOELREGELAAR (ff_auto + ff_water in koeling modus)
// ═══════════════════════════════════════════════════════════════

static void pas_ff_koel_aan(bool is_water){
  uint32_t nu = millis();

  // Minimale buitentemp voor koeling — anders aborteer en schakel koeling uit
  if(t_outside < KOELING_MIN_BUITEN){
    if(ctrl.wp_aan){ ctrl.wp_uit_ms = nu; ctrl.zet_uit(); }
    koeling_modus = false;
    stuur_alert("Koeling gestopt: buiten " + String(t_outside,1) + "C < min " + String(KOELING_MIN_BUITEN,1) + "C");
    return;
  }

  // Koelsetpoint: ff_water → t_water_gewenst (bijv. 18°C), ff_auto → kamer
  float wsp = is_water ? t_water_gewenst : t_kamer_gewenst;
  doel_setpoint = wsp;

  // Regelafwijking koeling: positief = nog te warm = meer koeling nodig
  float regel_fout = is_water ? (t_supply - wsp) : (t_kamer - t_kamer_gewenst);

  // Condensatiebescherming: aanvoer mag niet onder SUPPLY_MIN komen
  if(t_supply <= SUPPLY_MIN + 0.2f){
    if(ctrl.stand > 0 && (nu - ctrl.vorige_stand_wijz_ms >= (uint32_t)HYST_DOWN_MS)){
      ctrl.stand--;
      ctrl.vorige_stand_wijz_ms = nu;
      ctrl.wp_aan = (ctrl.stand > 0);
      if(ctrl.stand == 0){ ctrl.wp_uit_ms = nu; ctrl.reset_ff(); }
    }
    return;
  }

  // Te koud: terugschakelen (kamer/aanvoer heeft setpoint onderschreden)
  if(regel_fout < -KOELING_AFSCHAKEL){
    if(ctrl.stand > 0 && (nu - ctrl.vorige_stand_wijz_ms >= (uint32_t)HYST_DOWN_MS)){
      ctrl.stand--;
      ctrl.vorige_stand_wijz_ms = nu;
      ctrl.wp_aan = (ctrl.stand > 0);
      if(ctrl.stand == 0){ ctrl.wp_uit_ms = nu; ctrl.reset_ff(); }
    }
    return;
  }

  // Herstart vanuit stand 0: min. wachttijd + voldoende warmtevraag
  if(ctrl.stand == 0){
    bool min_off_ok = (ctrl.wp_uit_ms == 0) || ((nu - ctrl.wp_uit_ms) >= (uint32_t)FF_THERMAL_MIN_OFF_MS);
    bool drempel_ok = (regel_fout > FF_RESTART_COAST);
    if(!min_off_ok || !drempel_ok) return;
  }

  // FF: benodigde koelvermogen
  float P_nodig, cop;
  if(is_water){
    // Warmtestroom kamer → koud water (emitter model)
    P_nodig = max(0.0f, ff_UA_emitter * (t_kamer - wsp));
    cop = ff_cop_koel(t_supply, t_outside);
  } else {
    // Warmte-inval vanuit buiten die weggekoeeld moet worden (huis model)
    P_nodig = max(0.0f, ff_UA_house * (t_outside - t_kamer_gewenst));
    cop = ff_cop_koel(t_supply, t_outside);
  }

  uint8_t stand_ff = ff_stand_voor_vermogen(P_nodig / max(cop, 1.0f));

  float coast_k = is_water ? FF_COAST_WATER : FF_COAST_AUTO;
  if(regel_fout > coast_k) stand_ff = min(8, (int)stand_ff + 1);

  // Integraalcorrectie (zelfde logica als verwarming)
  float ff_ki = is_water ? FF_KI_WATER : FF_KI_AUTO;
  float integraal_zone = is_water ? 2.0f : 1.0f;
  if(ctrl.stand > 0 && fabsf(regel_fout) < integraal_zone)
    ctrl.ff_integraal += regel_fout * (pid_interval_ms / 1000.0f);
  float ff_max_int = 3.0f * 3600.0f / ff_ki;
  ctrl.ff_integraal = constrain(ctrl.ff_integraal, -ff_max_int, ff_max_int);
  int8_t int_corr = (int8_t)constrain((int)(ctrl.ff_integraal * ff_ki / 3600.0f), -2, 2);
  if(regel_fout <= 0.0f && int_corr > 0) int_corr = 0;

  int max_stap = (regel_fout > 3.0f) ? 3 : 1;
  int nieuwe_stand_i = constrain((int)stand_ff + int_corr, 0, 8);
  if(ctrl.stand > 0)
    nieuwe_stand_i = constrain(nieuwe_stand_i,
                               max(0, (int)ctrl.stand - max_stap),
                               min(8, (int)ctrl.stand + max_stap));
  uint8_t nieuwe_stand = (uint8_t)nieuwe_stand_i;
  ctrl.pid_output = stand_ff * 12.5f;

  long hyst;
  if(nieuwe_stand < ctrl.stand)  hyst = HYST_DOWN_MS;
  else if(regel_fout > coast_k)  hyst = HYST_FAST_MS;
  else                           hyst = HYST_SLOW_MS;

  if(nieuwe_stand != ctrl.stand && (nu - ctrl.vorige_stand_wijz_ms >= (uint32_t)hyst)){
    ctrl.stand = nieuwe_stand;
    ctrl.vorige_stand_wijz_ms = nu;
    ctrl.wp_aan = (ctrl.stand > 0);
    if(ctrl.stand == 0){ ctrl.wp_uit_ms = nu; ctrl.reset_ff(); }
    mqtt_log(String(is_water ? "FF-W koel" : "FF-A koel") +
             ": fout=" + String(regel_fout,1) +
             " ff=" + String(stand_ff) +
             " St=" + String(ctrl.stand), "INFO");
  }
}

// ═══════════════════════════════════════════════════════════════
//  FEEDFORWARD REGELAAR (ff_auto / ff_water)
// ═══════════════════════════════════════════════════════════════

void pas_ff_aan(){
  bool is_water = (modus == Modus::FF_WATER);
  uint32_t nu = millis();

  // ── Koelmodus: door naar aparte regelaar ──────────────────────
  if(koeling_modus){ pas_ff_koel_aan(is_water); return; }

  // ── Stooklijn berekenen (ff_auto) / doel-aanvoer (ff_water) ──
  float stooklijn = setpoint;
  if(t_outside < STOOKLIJN_GRENS)
    stooklijn = min(45.0f, setpoint + (STOOKLIJN_GRENS - t_outside) * STOOKLIJN_FACTOR);
  // ff_water: extern opgelegd setpoint (Adam); ff_auto: stooklijn
  float wsp = is_water ? t_water_gewenst : stooklijn;
  doel_setpoint = wsp;

  // ── Adam/extern: geen warmtevraag (SP=0) → WP uit, tenzij vorst ─
  if(is_water && t_water_gewenst == 0.0f){
    if(t_outside < T_VORST){
      if(ctrl.stand != 1 || !ctrl.wp_aan){
        ctrl.stand = 1; ctrl.wp_aan = true; ctrl.vorige_stand_wijz_ms = nu;
        mqtt_log("VORSTBEVEILIGING (SP=0): buiten " + String(t_outside,1) + "C", "WARNING");
      }
    } else {
      if(ctrl.wp_aan){ ctrl.wp_uit_ms = nu; ctrl.wp_thermal_stop = false; ctrl.zet_uit(); }
    }
    return;
  }

  // ── Buiten seizoen: WP uit (alleen ff_auto) ───────────────────
  // [A] Hysteresis: uit bij STOOKLIJN_UIT_GRENS, aan pas bij STOOKLIJN_AAN_GRENS
  if(!is_water){
    if(t_outside > STOOKLIJN_UIT_GRENS){
      if(ctrl.wp_aan){ ctrl.wp_uit_ms = nu; ctrl.wp_thermal_stop = false; ctrl.zet_uit(); }
      return;
    }
    // Wacht met herstarten tot t_outside ≤ STOOKLIJN_AAN_GRENS (of WP al aan, of laatste stop was thermisch)
    if(!ctrl.wp_aan && t_outside > STOOKLIJN_AAN_GRENS && !ctrl.wp_thermal_stop) return;
  }

  // ── Regelafwijking ─────────────────────────────────────────────
  float regel_fout = is_water ? (wsp - t_supply) : (t_kamer_gewenst - t_kamer);
  float afschakeldrempel = is_water ? -1.5f : FF_AFSCHAKEL_AUTO;

  // ── Predictieve terugschakeling (alleen ff_auto) ───────────────
  // Bereken afgeleide van kamertemp over een sliding window van 20 min
  // (past bij τ_huis ≈ 13 uur; filters meetruis eruit).
  // Sample 1× per minuut in ringbuffer; extrapoleer FF_LOOKAHEAD_MS vooruit.
  float regel_fout_eff = regel_fout;
  if(!is_water && FF_LOOKAHEAD_MS > 0){
    // Voeg 1× per minuut een nieuw sample toe
    if(ctrl.deriv_count == 0 || (nu - ctrl.deriv_last_ms) >= 60000UL){
      ctrl.deriv_buf[ctrl.deriv_idx] = t_kamer;
      ctrl.deriv_idx   = (ctrl.deriv_idx + 1) % ControllerState::DERIV_BUF_N;
      if(ctrl.deriv_count < ControllerState::DERIV_BUF_N) ctrl.deriv_count++;
      ctrl.deriv_last_ms = nu;
    }
    // Helling over het volledige gevulde venster (oudste → huidige meting)
    if(ctrl.deriv_count >= 2){
      uint8_t oldest_idx = (ctrl.deriv_idx + ControllerState::DERIV_BUF_N - ctrl.deriv_count)
                           % ControllerState::DERIV_BUF_N;
      float kamer_old = ctrl.deriv_buf[oldest_idx];
      float dt_s      = (ctrl.deriv_count - 1) * 60.0f;  // venster in seconden
      float kamer_dt  = (t_kamer - kamer_old) / dt_s;    // °C/s stijgsnelheid
      float kamer_pred = t_kamer + kamer_dt * (FF_LOOKAHEAD_MS / 1000.0f);
      regel_fout_eff   = t_kamer_gewenst - kamer_pred;
    }
  }

  // ── FF_AUTO herstart-beperking (alleen bij stand 0) ────────────
  // [B+C] Kortere wachttijd na thermische stop (overshoot) dan na seizoensstop:
  //  - Thermisch: FF_THERMAL_MIN_OFF_MS (1 min) — kamer moet alleen ff_restart_coast dalen
  //  - Seizoen:   FF_MIN_OFF_MS (10 min) — WP was seizoensmatig uit
  if(!is_water && ctrl.stand == 0){
    long min_off     = ctrl.wp_thermal_stop ? FF_THERMAL_MIN_OFF_MS : FF_MIN_OFF_MS;
    bool min_off_ok  = (ctrl.wp_uit_ms == 0) || ((nu - ctrl.wp_uit_ms) >= (uint32_t)min_off);
    bool fout_ok     = (regel_fout > FF_RESTART_COAST);
    if(!min_off_ok || !fout_ok) return;
  }

  // ── Te warm: uitschakelen (of minimum bij vorst) ──────────────
  if(regel_fout_eff < afschakeldrempel){
    if(t_outside < T_VORST){
      // Vorstbeveiliging geldt voor alle FF-modi: nooit onder stand 1
      if(ctrl.stand != 1 || !ctrl.wp_aan){
        ctrl.stand = 1; ctrl.wp_aan = true; ctrl.vorige_stand_wijz_ms = nu;
      }
    } else if(ctrl.stand > 0 && (nu - ctrl.vorige_stand_wijz_ms >= (uint32_t)HYST_DOWN_MS)){
      // Bij grote overshoot meer stappen tegelijk: COP is dan hoog en elke
      // stand levert nog netto warmte, dus 1 stap/min is niet snel genoeg.
      int stap = (-regel_fout > 4.0f) ? 2 : 1;
      ctrl.stand = (uint8_t)max(0, (int)ctrl.stand - stap);
      ctrl.vorige_stand_wijz_ms = nu;
      ctrl.wp_aan = (ctrl.stand > 0);
      if(ctrl.stand == 0){ ctrl.wp_uit_ms = nu; ctrl.wp_thermal_stop = true; ctrl.reset_ff(); }
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
  // Alleen accumuleren dichtbij setpoint: voorkomt windup tijdens opwarmfase.
  // Ver van setpoint (|fout| > integraal_zone) loopt de integraal niet op,
  // zodat het systeem de aankomstfase niet met +2 standen overschiet.
  float ff_ki = is_water ? FF_KI_WATER : FF_KI_AUTO;
  float integraal_zone = is_water ? 2.0f : 1.0f;
  // Alleen integreren als WP draait: voorkomt dat integraal opbouwt tijdens stilstand
  // en bij herstart direct een te hoge stand geeft die weer overshoot veroorzaakt.
  if(ctrl.stand > 0 && fabsf(regel_fout) < integraal_zone)
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
  if(t_outside < T_VORST) nieuwe_stand_i = max(1, nieuwe_stand_i);
  // Bij koude start (stand was 0): direct naar evenwichtsstand, geen stapbeperking.
  // Voorkomt trage 0→1→2→... opbouw als kamer al op setpoint staat.
  if(ctrl.stand > 0)
    nieuwe_stand_i = constrain(nieuwe_stand_i, max(0, (int)ctrl.stand - max_stap), min(8, (int)ctrl.stand + max_stap));
  uint8_t nieuwe_stand = (uint8_t)nieuwe_stand_i;

  // ── Online leren ───────────────────────────────────────────────
  // Alleen bij thermisch evenwicht (klein setpoint-fout): anders absorbeert
  // het model de thermische-massa term (C_th × dT/dt) als extra UA.
  float leer_drempel = is_water ? 2.0f : 0.5f;
  float P_hp_est = VERMOGEN[ctrl.stand] * ff_cop(t_supply, t_outside);
  // Leer-gate multizone: bij bron==ADAM mag ff_UA_emitter alleen leren als de
  // leidende Adam-zone stabiel is (anders koppelt het een water-SP van zone B
  // aan de temperatuur van zone A → vervuilde UA). Zie docs/ADAM_INTEGRATIE.md §4b.
  bool emitter_leren_ok = (bron != Bron::ADAM) || adam_leer_emitter_ok;
  if(ctrl.stand > 0 && P_hp_est > 50.0f && fabsf(regel_fout) < leer_drempel){
    if(is_water){
      float dt_sup = t_supply - t_kamer;
      if(dt_sup > 2.0f && emitter_leren_ok){
        float meting = P_hp_est / dt_sup;
        ff_UA_emitter = (1.0f - FF_LEARN_RATE) * ff_UA_emitter + FF_LEARN_RATE * meting;
        ff_UA_emitter = constrain(ff_UA_emitter, 50.0f, 500.0f);
      }
    } else {
      // Minimale ΔT=10°C: bij kleine ΔT (zachte buitentemperatuur) is de
      // UA-meting 4× ruis-gevoeliger → UA loopt weg. Alleen leren bij
      // voldoende ΔT_kamer_buiten zodat de schatting P/ΔT stabiel is.
      float dt_env = t_kamer - t_outside;
      if(dt_env > 10.0f){
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
    if(ctrl.stand == 0){ ctrl.wp_uit_ms = nu; ctrl.wp_thermal_stop = true; ctrl.reset_ff(); }
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
  if(koeling_modus && (modus == Modus::AUTO || modus == Modus::WATER)){
    // Koeling niet toegestaan in AUTO en WATER — automatisch uitzetten
    koeling_modus = false; ctrl.reset_pid();
    stuur_alert("Koeling alleen in FF_AUTO/FF_WATER/HANDMATIG — koeling uitgeschakeld");
  }
  if(koeling_modus && t_outside < KOELING_MIN_BUITEN){
    koeling_modus = false; ctrl.reset_pid();
    stuur_alert("Koeling geblokkeerd: buiten " + String(t_outside,1) + "C < min " + String(KOELING_MIN_BUITEN,1) + "C");
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
    // Adam/extern: geen warmtevraag (SP=0) → WP uit, tenzij vorst
    if(t_water_gewenst == 0.0f){
      if(t_outside < T_VORST){
        if(ctrl.stand != 1 || !ctrl.wp_aan){
          ctrl.stand = 1; ctrl.wp_aan = true; ctrl.vorige_stand_wijz_ms = nu;
          mqtt_log("VORSTBEVEILIGING (SP=0): buiten " + String(t_outside,1) + "C", "WARNING");
        }
      } else {
        if(ctrl.wp_aan){ ctrl.zet_uit(); mqtt_log("WATER: geen vraag (SP=0) -> WP UIT", "INFO"); }
      }
      return;
    }
    float water_fout = t_water_gewenst - t_supply;
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
      ctrl.pid_output = constrain(Kp_water * water_fout + Ki_water * ctrl.pid_integraal + Kd_water * diff, -100.0f, 100.0f);

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
  if(kamer_fout > AUTO_AAN_DREMPEL){
    if(!ctrl.wp_aan) ctrl.wp_start_ms = nu;  // onthoud wanneer WP aansloeg
    ctrl.wp_aan = true;
    float aanvoer_fout = doel_setpoint - t_supply;
    float dt_correctie = 0;
    if(delta_t < 4.0)      dt_correctie = (delta_t - 5.0) * 3.0;
    else if(delta_t > 6.0) dt_correctie = (delta_t - 5.0) * 2.0;

    float diff = (aanvoer_fout - ctrl.pid_vorige_fout) / 0.005;
    ctrl.pid_vorige_fout = aanvoer_fout;
    float pid_output_raw = Kp * aanvoer_fout + Ki * ctrl.pid_integraal + Kd * diff + dt_correctie;
    // Anti-windup: integreer alleen als output niet gesatureerd is in de windrichting
    if(!((pid_output_raw > 100.0f && aanvoer_fout > 0.0f) ||
         (pid_output_raw <   0.0f && aanvoer_fout < 0.0f))){
      ctrl.pid_integraal += aanvoer_fout * 0.005;
      ctrl.pid_integraal = constrain(ctrl.pid_integraal, -50.0f, 50.0f);
    }
    ctrl.pid_output = constrain(pid_output_raw, 0.0f, 100.0f);

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

    // [A] Deadband: geen standwijziging als aanvoer al dicht bij setpoint
    if(fabsf(aanvoer_fout) < AUTO_AANVOER_DEADBAND)
      nieuwe_stand = ctrl.stand;

    // [B] ±1 staplimiet per cycle: geen grote sprongen (bijv. 0→8)
    if(nieuwe_stand > ctrl.stand + 1) nieuwe_stand = ctrl.stand + 1;
    if(nieuwe_stand < ctrl.stand - 1) nieuwe_stand = ctrl.stand - 1;

    // [C] Lineaire coldstart-ramp over 30 minuten (was: harde grens van 3 na 15 min)
    uint32_t elapsed_start = nu - ctrl.wp_start_ms;
    if(elapsed_start < 1800000UL){
      uint8_t max_ramp = (uint8_t)max(1, (int)(elapsed_start / 1800000.0f * 8) + 1);
      if(nieuwe_stand > max_ramp) nieuwe_stand = max_ramp;
    }

    if(t_outside < T_VORST && nieuwe_stand == 0) nieuwe_stand = 1;
    long hyst = (kamer_fout > 1.0f) ? HYST_FAST_MS : HYST_SLOW_MS;
    if(nieuwe_stand != ctrl.stand && (nu - ctrl.vorige_stand_wijz_ms >= (uint32_t)hyst)){
      ctrl.stand = nieuwe_stand; ctrl.vorige_stand_wijz_ms = nu;
      Serial.print("AUTO: kamer=");Serial.print(t_kamer,1);
      Serial.print(" fout=");Serial.print(kamer_fout,1);
      Serial.print(" St=");Serial.println(ctrl.stand);
    }
  } else if(kamer_fout < AUTO_UIT_DREMPEL){
    if(t_outside < T_VORST){
      if(ctrl.stand > 1){ ctrl.stand = 1; ctrl.wp_aan = true; ctrl.vorige_stand_wijz_ms = nu; }
    } else if(ctrl.stand > 0 && (nu - ctrl.vorige_stand_wijz_ms >= (uint32_t)AUTO_HYST_DOWN_MS)){
      ctrl.stand--;
      ctrl.vorige_stand_wijz_ms = nu;
      ctrl.wp_aan = (ctrl.stand > 0);
      if(ctrl.stand == 0){
        ctrl.soft_reset_pid();  // [D] halveer integraal (was: harde reset naar 0)
        mqtt_log("Kamer te warm (" + String(t_kamer,1) + "C) -> WP UIT", "INFO");
      }
    }
  }
}
