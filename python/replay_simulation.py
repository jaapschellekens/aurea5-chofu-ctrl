#!/usr/bin/env python3
"""
Chofu WP simulatie — replay historische buitentemperaturen.

Een thermisch huismodel (HouseModel) berekent kamer- en aanvoertemperatuur.
Een Python-spiegel van de Arduino PID-regelaar (ArduinoPID) bepaalt de
compressorstand. Volledig offline — geen MQTT of Arduino nodig.

Gebruik:
    python replay_simulation.py [opties]

Vereist: pip install pandas matplotlib
"""

import argparse
import math
import os
import sys
from datetime import datetime

import pandas as pd


# ── Configuratie ───────────────────────────────────────────────────────────
CSV_FILE   = "./history(4).csv"
OUTPUT_DIR = "sim_output"

# PID / hysteresis productiewaarden (in milliseconden, zoals in de Arduino sketch)
HYST_SLOW_PROD = 600_000   # 10 minuten
HYST_FAST_PROD = 120_000   #  2 minuten
HYST_DOWN_PROD = 300_000   #  5 minuten
PID_INT_PROD   =   5_000   #  5 seconden

# Vermogen per stand (W) — overeenkomstig de Arduino sketch
VERMOGEN = [0, 240, 420, 640, 850, 1050, 1250, 1450, 1550, 1650, 1700, 1750, 1800]

# CSV-kolommen formaat v1 (history.csv)
ENTITY_MAP = {
    "sensor.cresta_4802_temp":                      "chofu/sim/outside",
    "sensor.anna_setpoint":                         "chofu/cmd/kamer_setpoint",
    "sensor.opentherm_intended_boiler_temperature": "chofu/sim/water_setpoint",
}
REFERENCE_MAP = {
    "sensor.woonkamer_temperatuur":                 "historisch/kamer",
    "sensor.opentherm_water_temperature":           "historisch/aanvoer",
    "sensor.opentherm_intended_boiler_temperature": "historisch/water_setpoint",
}
OBSERVE_MAP = {
    "sensor.tz3000_g5xawfcq_ts0121_vermogen_2": "historisch/vermogen_w",
}

# CSV-kolommen formaat v2 (history(2).csv)
# climate.woonkamer heeft current_temperature en temperature als aparte kolommen
ENTITY_MAP_V2 = {
    "sensor.cresta_4802_temp":                      "chofu/sim/outside",
    "sensor.anna_setpoint":                         "chofu/cmd/kamer_setpoint",
    "climate.woonkamer_setpoint":                   "chofu/cmd/kamer_setpoint",
    "sensor.opentherm_intended_boiler_temperature": "chofu/sim/water_setpoint",
}
REFERENCE_MAP_V2 = {
    "sensor.cresta_6703_temp":                      "historisch/kamer",
    "sensor.opentherm_water_temperature":           "historisch/aanvoer",
    "sensor.opentherm_return_temperature":          "historisch/retour",
    "sensor.opentherm_intended_boiler_temperature": "historisch/water_setpoint",
}
OBSERVE_MAP_V2 = {}


def _preprocess_history2(df):
    """
    Normaliseer history(2).csv naar entity_id/state/last_changed formaat.
    climate.woonkamer heeft current_temperature en temperature als aparte kolommen.
    """
    rows = []
    climate = df[df["entity_id"] == "climate.woonkamer"].copy()
    if not climate.empty:
        r = climate[["last_changed"]].copy()
        r["entity_id"] = "climate.woonkamer_kamer"
        r["state"] = pd.to_numeric(climate["current_temperature"].values, errors="coerce")
        rows.append(r.dropna(subset=["state"]))

        r = climate[["last_changed"]].copy()
        r["entity_id"] = "climate.woonkamer_setpoint"
        r["state"] = pd.to_numeric(climate["temperature"].values, errors="coerce")
        rows.append(r.dropna(subset=["state"]))

    other = df[df["entity_id"] != "climate.woonkamer"][["entity_id", "state", "last_changed"]].copy()
    rows.append(other)
    return pd.concat(rows, ignore_index=True)


# ── Thermisch huismodel ────────────────────────────────────────────────────
class HouseModel:
    """
    Simpel thermisch model voor 160m² vrijstaand houtskeletbouw huis met HR++ glas.

    Twee gekoppelde energiebalansen:
      1. Watertemperatuur: warmtepomp vermogen vs. warmteafgifte via radiatoren
      2. Kamertemperatuur: warmteafgifte vs. transmissie- en ventilatieverliezen

    Standaard parameters voor 160m² vrijstaand HBH huis, HR++ glas,
    LT-radiatoren met geforceerde ventilatie:

      UA_house   = 100 W/K  →  ontwerplast @ -10°C: 3 kW
      C_th       = 3.6 MJ/K →  tijdconstante τ ≈ 10 uur
      UA_emitter = 200 W/K  →  vermogen @ 35/20°C: 2.5 kW
      C_water    = 200 kJ/K →  ≈ 50 L water (leidingen + radiatoren), τ ≈ 17 min
      COP_eta    = 0.40     →  COP ≈ 3.5 @ 35°C aanvoer / 0°C buiten
    """

    def __init__(self, UA_house=263.0, C_th=12.5e6, UA_emitter=344.0,
                 C_water=200e3, COP_eta=0.40,
                 t_kamer_init=20.0, t_supply_init=35.0):
        self.UA_house   = UA_house
        self.C_th       = C_th
        self.UA_emitter = UA_emitter
        self.C_water    = C_water
        self.COP_eta    = COP_eta

        self._t_kamer_init  = t_kamer_init
        self._t_supply_init = t_supply_init
        self.reset()

    def reset(self):
        """Zet thermische toestand terug naar beginwaarden (nodig bij herhaalde simulatie)."""
        self.t_kamer    = self._t_kamer_init
        self.t_supply   = self._t_supply_init
        self.t_return   = self._t_supply_init - 5.0
        self.cop        = 0.0
        self.p_to_house = 0.0

    def calc_cop(self, t_outside):
        T_s = self.t_supply + 273.15
        T_o = t_outside + 273.15
        dT  = T_s - T_o
        if dT < 1.0:
            return 1.0
        return max(1.0, min(6.0, (T_s / dT) * self.COP_eta))

    def step(self, t_outside, stand, dt_s):
        """
        Simuleer één tijdstap van dt_s seconden.

        Water: exacte analytische oplossing (exp-verval naar evenwicht) — stabiel
        voor elke dt ongeacht τ_water = C_water / UA_emitter.
        Huis:  Forward-Euler met tijdsgemiddelde p_to_house over de stap;
        geldig omdat τ_huis ≫ dt_s.

        Emitter-correctie: het originele model rekende met t_mean = (t_sup + t_ret)/2
        en G_flow = 600 W/K. Dit geeft een effectieve UA bij aanvoertemperatuur:
          UA_eff = 2·G·UA_emitter / (2·G + UA_emitter)
        Het steady-state warmteaanbod is daarmee identiek aan het originele model.
        """
        G_FLOW = 600.0   # W/K  (= m_dot × Cp voor het afgifte-circuit)

        stand      = max(0, min(12, int(stand)))
        P_electric = VERMOGEN[stand]

        self.cop = self.calc_cop(t_outside) if stand > 0 else 0.0
        P_hp     = P_electric * self.cop

        # Effectieve UA bij gebruik van aanvoertemperatuur (ipv gemiddelde)
        ua_eff = 2.0 * G_FLOW * self.UA_emitter / (2.0 * G_FLOW + self.UA_emitter)
        tau    = self.C_water / ua_eff   # tijdconstante water [s]

        # Evenwichtstemperatuur bij huidige stand
        t_ss  = self.t_kamer + P_hp / ua_eff
        exp_f = math.exp(-dt_s / tau)

        # Exacte eindtemperatuur water na dt_s
        t_supply_new = t_ss + (self.t_supply - t_ss) * exp_f
        t_supply_new = max(self.t_kamer, t_supply_new)

        # Tijdsgemiddelde T_water over de stap → correct warmteaanbod aan huis
        # T_avg = T_ss + (T_0 − T_ss) × (τ/dt) × (1 − exp(−dt/τ))
        t_supply_avg    = t_ss + (self.t_supply - t_ss) * (tau / dt_s) * (1.0 - exp_f)
        self.p_to_house = max(0.0, ua_eff * (t_supply_avg - self.t_kamer))

        # Retourtemperatuur (aanvoer minus aanvoer/retour split)
        delta_T       = self.p_to_house / G_FLOW if self.p_to_house > 50.0 else 2.0
        self.t_return = max(self.t_kamer - 1.0, t_supply_new - delta_T)

        # Huis: Forward-Euler is stabiel (τ_huis ≫ dt_s)
        P_loss        = self.UA_house * (self.t_kamer - t_outside)
        self.t_kamer += (self.p_to_house - P_loss) / self.C_th * dt_s

        self.t_supply = t_supply_new


# ── ArduinoPID spiegel ────────────────────────────────────────────────────
class ArduinoPID:
    """
    Python-spiegel van Arduino pas_pid_aan() — exact dezelfde logica.

    Tijden in gesimuleerde seconden (productiewaarden: hyst_slow=600s, etc.).
    """

    def __init__(self, modus="auto",
                 Kp=0.8, Ki=0.01, Kd=0.3,
                 kamer_corr_normaal=20.0, kamer_corr_groot=30.0,
                 setpoint=28.0, stooklijn_grens=15.0, stooklijn_factor=0.68,
                 t_vorst=4.0, supply_max=60.0, stooklijn_uit=15.0,
                 hyst_slow_s=600.0, hyst_fast_s=120.0, hyst_down_s=300.0,
                 pid_interval_s=5.0, max_stand_stap=1):
        self.modus               = modus
        self.Kp                  = Kp
        self.Ki                  = Ki
        self.Kd                  = Kd
        self.kamer_corr_normaal  = kamer_corr_normaal
        self.kamer_corr_groot    = kamer_corr_groot
        self.setpoint            = setpoint
        self.stooklijn_grens  = stooklijn_grens
        self.stooklijn_factor = stooklijn_factor
        self.t_vorst          = t_vorst
        self.supply_max       = supply_max
        self.stooklijn_uit    = stooklijn_uit
        self.hyst_slow_s      = hyst_slow_s
        self.hyst_fast_s      = hyst_fast_s
        self.hyst_down_s      = hyst_down_s
        self.pid_interval_s   = pid_interval_s
        self.max_stand_stap   = max_stand_stap

        self.pid_integraal    = 0.0
        self.pid_vorige_fout  = 0.0
        self.pid_output       = 0.0
        self.stand            = 0
        self.wp_aan           = False
        self.doel_setpoint    = setpoint

        self._sim_s        = 0.0
        self._last_pid_s   = -pid_interval_s
        self._last_stand_s = -(hyst_slow_s + 1.0)
        self._pid_first_step = True  # eerste diff = 0 zodat er geen startup-kick is

    def _pid_to_stand(self, pid_pct):
        p = pid_pct
        if p < 5:   return 0
        if p < 15:  return 1
        if p < 25:  return 2
        if p < 40:  return 3
        if p < 55:  return 4
        if p < 70:  return 5
        if p < 85:  return 6
        if p < 93:  return 7
        return 8

    def step(self, dt_s, t_outside, t_supply, t_return,
             t_kamer, t_kamer_gewenst, t_water_gewenst=40.0, p_to_house=0.0):
        """Zet simulatie dt_s seconden verder. Geeft stand terug (0-8)."""
        self._sim_s += dt_s
        nu = self._sim_s

        if t_supply > self.supply_max:
            self.stand = 0
            self.wp_aan = False
            self.pid_integraal = 0.0
            return self.stand

        if nu - self._last_pid_s < self.pid_interval_s:
            return self.stand
        self._last_pid_s = nu

        if self.modus == "water":
            return self._step_water(nu, t_outside, t_supply, t_water_gewenst)
        return self._step_auto(nu, t_outside, t_supply, t_return,
                               t_kamer, t_kamer_gewenst)

    def _step_auto(self, nu, t_outside, t_supply, t_return, t_kamer, t_kamer_gewenst):
        if t_outside > self.stooklijn_uit:
            if self.wp_aan:
                self.wp_aan = False
                self.stand = 0
                self.pid_integraal = 0.0
            return self.stand

        kamer_fout = t_kamer_gewenst - t_kamer

        self.doel_setpoint = self.setpoint
        if t_outside < self.stooklijn_grens:
            self.doel_setpoint = min(
                45.0,
                self.setpoint + (self.stooklijn_grens - t_outside) * self.stooklijn_factor,
            )

        if t_outside < self.t_vorst and self.stand == 0:
            self.stand = 1
            self.wp_aan = True
            self._last_stand_s = nu

        if t_kamer > min(t_kamer_gewenst + 0.5, 25.0):
            self.wp_aan = False
            self.stand = 0
            self.pid_integraal = 0.0
            return self.stand

        if kamer_fout > 0.1:
            self.wp_aan = True
            aanvoer_fout = self.doel_setpoint - t_supply

            delta_t = t_supply - t_return
            dt_correctie = 0.0
            if delta_t < 4.0:
                dt_correctie = (delta_t - 5.0) * 3.0
            elif delta_t > 6.0:
                dt_correctie = (delta_t - 5.0) * 2.0

            kamer_correctie = kamer_fout * (
                self.kamer_corr_groot if kamer_fout > 1.5 else self.kamer_corr_normaal
            )

            self.pid_integraal += aanvoer_fout * 0.005
            self.pid_integraal = max(-50.0, min(50.0, self.pid_integraal))
            if self._pid_first_step:
                self.pid_vorige_fout = aanvoer_fout
                self._pid_first_step = False
            diff = (aanvoer_fout - self.pid_vorige_fout) / 0.005
            self.pid_vorige_fout = aanvoer_fout

            self.pid_output = (self.Kp * aanvoer_fout
                               + self.Ki * self.pid_integraal
                               + self.Kd * diff
                               + dt_correctie + kamer_correctie)
            if kamer_fout > 1.5 and self.pid_output < 55.0:
                self.pid_output = 55.0
            self.pid_output = max(0.0, min(100.0, self.pid_output))

            nieuwe_stand = self._pid_to_stand(self.pid_output)
            if t_outside < self.t_vorst and nieuwe_stand == 0:
                nieuwe_stand = 1
            if self.max_stand_stap > 0:
                nieuwe_stand = max(self.stand - self.max_stand_stap,
                                   min(self.stand + self.max_stand_stap, nieuwe_stand))

            if nieuwe_stand < self.stand:
                hyst = self.hyst_down_s
            elif kamer_fout > 1.0:
                hyst = self.hyst_fast_s
            else:
                hyst = self.hyst_slow_s

            if nieuwe_stand != self.stand and (nu - self._last_stand_s >= hyst):
                self.stand = nieuwe_stand
                self._last_stand_s = nu
                if self.stand == 0:
                    self.wp_aan = False
                    self.pid_integraal = 0.0

        elif kamer_fout < -0.2:
            if t_outside < self.t_vorst:
                if self.stand > 1:
                    self.stand = 1
                    self.wp_aan = True
            else:
                self.wp_aan = False
                self.stand = 0
                self.pid_integraal = 0.0
                self._last_stand_s = nu  # voorkomt direct herstarten na uitschakelen

        return self.stand

    def _step_water(self, nu, t_outside, t_supply, t_water_gewenst):
        water_fout = t_water_gewenst - t_supply
        self.doel_setpoint = t_water_gewenst

        if t_outside < self.t_vorst and self.stand == 0:
            self.stand = 1
            self.wp_aan = True
            self._last_stand_s = nu

        if water_fout > 1.0:
            self.wp_aan = True
        elif water_fout < -1.0:
            if t_outside >= self.t_vorst:
                self.wp_aan = False
                self.stand = 0
                self.pid_integraal = 0.0
                return self.stand

        if self.wp_aan:
            self.pid_integraal += water_fout * 0.005
            self.pid_integraal = max(-50.0, min(50.0, self.pid_integraal))
            if self._pid_first_step:
                self.pid_vorige_fout = water_fout
                self._pid_first_step = False
            diff = (water_fout - self.pid_vorige_fout) / 0.005
            self.pid_vorige_fout = water_fout

            self.pid_output = min(100.0,
                                  self.Kp * water_fout
                                  + self.Ki * self.pid_integraal
                                  + self.Kd * diff)

            nieuwe_stand = self._pid_to_stand(self.pid_output)
            if t_outside < self.t_vorst and nieuwe_stand == 0:
                nieuwe_stand = 1
            if self.max_stand_stap > 0:
                nieuwe_stand = max(self.stand - self.max_stand_stap,
                                   min(self.stand + self.max_stand_stap, nieuwe_stand))

            if nieuwe_stand < self.stand:
                hyst = self.hyst_down_s
            else:
                # bij stapbeperking altijd hyst_slow zodat opwaartse trap ≥ 10 min duurt
                hyst = self.hyst_slow_s if self.max_stand_stap > 0 else (
                    self.hyst_fast_s if water_fout > 5.0 else self.hyst_slow_s)

            if nieuwe_stand != self.stand and (nu - self._last_stand_s >= hyst):
                self.stand = nieuwe_stand
                self._last_stand_s = nu
                if self.stand == 0:
                    self.wp_aan = False
                    self.pid_integraal = 0.0

        return self.stand


# ── Feedforward + lerende controller ──────────────────────────────────────
class FeedforwardController:
    """
    Feedforward controller op basis van de energiebalans van het huis.

    Aanpak:
      1. FF-stand: P_nodig = UA_huis × (T_set − T_buiten)
                  stand_ff = laagste stand waarvan VERMOGEN[s] ≥ P_nodig/COP
      2. Correctie: langzame integrator op kamerfout (±1 stand)
      3. Leren: UA_huis_schatting wordt online bijgewerkt via:
                UA_est = P_naar_huis / (T_kamer − T_buiten)

    Geen derivative → geen oscillatie bij trage thermische systemen.
    """

    def __init__(self, modus="auto",
                 UA_house=120.0, Ki_kamer=0.003,
                 ua_learn_rate=0.002, kamer_coast_k=1.5,
                 t_vorst=4.0, supply_max=60.0, stooklijn_uit=15.0,
                 hyst_slow_s=600.0, hyst_fast_s=120.0, hyst_down_s=300.0,
                 max_stand_stap=1, pid_interval_s=300.0,
                 UA_emitter_eff=171.0):
        self.modus           = modus
        self.UA_house        = UA_house
        self.UA_emitter_eff  = UA_emitter_eff
        self.Ki_kamer        = Ki_kamer
        self.ua_learn_rate  = ua_learn_rate
        self.kamer_coast_k  = kamer_coast_k   # anticipatiezone in °C
        self.t_vorst        = t_vorst
        self.supply_max     = supply_max
        self.stooklijn_uit  = stooklijn_uit
        self.hyst_slow_s    = hyst_slow_s
        self.hyst_fast_s    = hyst_fast_s
        self.hyst_down_s    = hyst_down_s
        self.max_stand_stap = max_stand_stap
        self.pid_interval_s = pid_interval_s

        self.stand         = 0
        self.wp_aan        = False
        self.pid_integraal = 0.0   # kamerfout-integraal in °C·s
        self.pid_output    = 0.0   # P_nodig in W (voor logging)
        self.doel_setpoint = 0.0
        self.UA_house_est    = UA_house       # live schatting (auto modus)
        self.UA_emitter_est  = UA_emitter_eff # live schatting (water modus)

        self._sim_s        = 0.0
        self._last_pid_s   = -pid_interval_s
        self._last_stand_s = -(hyst_slow_s + 1.0)

    def _cop(self, t_supply, t_outside):
        T_s = t_supply + 273.15
        T_o = t_outside + 273.15
        dT  = T_s - T_o
        if dT < 1.0:
            return 1.0
        return max(1.0, min(6.0, (T_s / dT) * 0.40))

    def _power_to_stand(self, P_electric):
        """Laagste stand waarvan elektrisch vermogen ≥ P_electric."""
        for s, v in enumerate(VERMOGEN):
            if v >= P_electric:
                return s
        return len(VERMOGEN) - 1

    def step(self, dt_s, t_outside, t_supply, _t_return,
             t_kamer, t_kamer_gewenst, t_water_gewenst=40.0,
             p_to_house=0.0):
        """Stap de controller dt_s seconden vooruit. Geeft stand (0-8) terug."""
        self._sim_s += dt_s
        nu = self._sim_s

        if t_supply > self.supply_max:
            self.stand     = 0
            self.wp_aan    = False
            self.pid_integraal = 0.0
            return self.stand

        if nu - self._last_pid_s < self.pid_interval_s:
            return self.stand
        self._last_pid_s = nu

        # ── Buiten seizoen: WP uit ─────────────────────────────────────────
        if t_outside > self.stooklijn_uit:
            self.stand     = 0
            self.wp_aan    = False
            self.pid_integraal = 0.0
            return self.stand

        # ── Selecteer regelvariabele op basis van modus ────────────────────
        # Auto:  stuur op kamerfout   (T_kamer_gewenst − T_kamer)
        # Water: stuur op waterfout   (T_water_gewenst − T_aanvoer)
        if self.modus == "water":
            regel_fout = t_water_gewenst - t_supply
            self.doel_setpoint = t_water_gewenst
        else:
            regel_fout = t_kamer_gewenst - t_kamer
            self.doel_setpoint = t_kamer_gewenst

        # ── Te warm: uitschakelen (of minimum bij vorst in auto modus) ───────
        # Water: breed doodsband (-1.5°C) — bij kleine stooklijn-dalingen niet uitschakelen
        afschakeldrempel = -1.5 if self.modus == "water" else -0.5
        if regel_fout < afschakeldrempel:
            if self.modus != "water" and t_outside < self.t_vorst and self.stand > 1:
                # Auto: vorstbescherming — WP minimaal op stand=1 houden
                self.stand = 1
                self.wp_aan = True
                self._last_stand_s = nu
            else:
                # Water: stooklijn-setpoint houdt leidingen al warm; gewoon uitschakelen
                # Auto buiten vorstgebied: ook uitschakelen
                self.stand     = 0
                self.wp_aan    = False
                self.pid_integraal = 0.0
                self._last_stand_s = nu
            return self.stand

        # ── Feedforward: equilibriumstand bepalen ────────────────────────────
        # Warmtebehoefte huis is altijd de basis.
        # In water modus: UA_emitter_est bepaalt de benodigde aanvoertemperatuur
        # → geeft correctere COP dan de huidige (mogelijk afwijkende) aanvoertemperatuur.
        P_nodig = max(0.0, self.UA_house_est * (t_kamer_gewenst - t_outside))
        if self.modus == "water" and P_nodig > 0.0:
            t_supply_needed = t_kamer + P_nodig / self.UA_emitter_est
            cop_ff = self._cop(t_supply_needed, t_outside)
        else:
            cop_ff = self._cop(t_supply, t_outside)
        stand_maintenance = self._power_to_stand(P_nodig / max(cop_ff, 1.0))
        self.pid_output   = P_nodig

        # ── Anticipatiezone: warm-up boost boven maintenance ──────────────
        if regel_fout > self.kamer_coast_k:
            stand_ff = stand_maintenance + 1
        else:
            stand_ff = stand_maintenance

        # ── Integraal op regelfout (langzame correctie: ±2 stand) ─────────
        self.pid_integraal += regel_fout * dt_s
        integraal_corr = int(self.pid_integraal * self.Ki_kamer / 3600.0)
        integraal_corr = max(-2, min(2, integraal_corr))

        nieuwe_stand = max(0, min(8, stand_ff + integraal_corr))
        if self.modus != "water" and t_outside < self.t_vorst:
            # Vorstbescherming alleen in auto modus
            nieuwe_stand = max(1, nieuwe_stand)
        if self.max_stand_stap > 0:
            nieuwe_stand = max(self.stand - self.max_stand_stap,
                               min(self.stand + self.max_stand_stap, nieuwe_stand))

        # ── Online leren ──────────────────────────────────────────────────
        if self.modus == "water":
            # Water: UA_emitter schatten uit p_to_house en aanvoer-kamer ΔT
            # p_to_house ≈ UA_emitter_eff × (T_supply − T_kamer) in steady-state
            dt_supply = t_supply - t_kamer
            if self.stand > 0 and p_to_house > 50.0 and dt_supply > 2.0:
                UA_em_meting = p_to_house / dt_supply
                self.UA_emitter_est = ((1.0 - self.ua_learn_rate) * self.UA_emitter_est
                                       + self.ua_learn_rate * UA_em_meting)
        else:
            # Auto: UA_huis schatten uit p_to_house en kamer-buiten ΔT
            delta_t_env = t_kamer - t_outside
            if self.stand > 0 and p_to_house > 50.0 and delta_t_env > 3.0:
                UA_meting = p_to_house / delta_t_env
                self.UA_house_est = ((1.0 - self.ua_learn_rate) * self.UA_house_est
                                     + self.ua_learn_rate * UA_meting)

        # ── Hysteresis ────────────────────────────────────────────────────
        if nieuwe_stand < self.stand:
            hyst = self.hyst_down_s
        elif self.modus == "water" and regel_fout > self.kamer_coast_k:
            hyst = self.hyst_fast_s   # water koelt snel, snelle reactie nodig
        else:
            hyst = self.hyst_slow_s
        if nieuwe_stand != self.stand and (nu - self._last_stand_s >= hyst):
            self.stand = nieuwe_stand
            self._last_stand_s = nu
            self.wp_aan = (self.stand > 0)
            if self.stand == 0:
                self.pid_integraal = 0.0

        return self.stand


# ── Argumenten ─────────────────────────────────────────────────────────────
def parse_args():
    p = argparse.ArgumentParser(
        description="Chofu WP simulatie replay met thermisch huismodel",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument("--csv",    default=CSV_FILE)
    p.add_argument("--output", default=OUTPUT_DIR)
    p.add_argument("--start",  default=None,
                   help="Startdatum filter bijv. '2026-02-01'")
    p.add_argument("--end",    default=None,
                   help="Einddatum filter bijv. '2026-03-01'")
    p.add_argument("--modus",          default="auto", choices=["water", "auto"],
                   help="Regelstrategie")
    p.add_argument("--kamer-setpoint", type=float, default=20.0,
                   help="Kamertemperatuur setpoint voor auto-modus als CSV geen "
                        "chofu/cmd/kamer_setpoint bevat [°C]")
    p.add_argument("--water-setpoint", type=float, default=None,
                   help="Vaste water setpoint voor water-modus [°C]; "
                        "standaard: stooklijn (28°C base, 0.68/°C, grens 15°C)")
    r = p.add_argument_group("PID regeling")
    r.add_argument("--kp",               type=float, default=0.8,
                   help="Proportionele gain")
    r.add_argument("--ki",               type=float, default=0.01,
                   help="Integrale gain")
    r.add_argument("--kd",               type=float, default=0.3,
                   help="Differentiële gain")
    r.add_argument("--max-stand-stap",    type=int,   default=1,
                   help="Max stand-wijziging per hysteresis-interval (0=onbeperkt)")
    r.add_argument("--kamer-corr-normaal", type=float, default=20.0,
                   help="Kamer-fout correctiefactor bij fout ≤ 1.5°C "
                        "(lager = minder agressief, probeer 5–8)")
    r.add_argument("--kamer-corr-groot",   type=float, default=30.0,
                   help="Kamer-fout correctiefactor bij fout > 1.5°C "
                        "(lager = minder agressief, probeer 10–15)")
    g = p.add_argument_group("Huismodel (160m² HBH vrijstaand, HR++)")
    g.add_argument("--ua",         type=float, default=120.0,
                   help="Totaal warmteverlies UA [W/K]")
    g.add_argument("--cth",        type=float, default=5.0e6,
                   help="Thermische massa [J/K]")
    g.add_argument("--ua-emitter", type=float, default=200.0,
                   help="Afgiftesysteem UA [W/K]")
    g.add_argument("--cwater",     type=float, default=200e3,
                   help="Thermische massa watersysteem [J/K] "
                        "(≈50 L water → τ≈17 min bij UA_emitter=200 W/K)")
    g.add_argument("--cop-eta",    type=float, default=0.40,
                   help="Carnot efficiency factor [-]")
    g.add_argument("--t-init",     type=float, default=20.0,
                   help="Begintemperatuur kamer [°C]")
    p.add_argument("--controller", default="pid", choices=["pid", "ff"],
                   help="pid = Arduino PID-spiegel  |  ff = feedforward + lerende UA")
    f = p.add_argument_group("Feedforward controller (--controller ff)")
    f.add_argument("--ff-ua",          type=float, default=120.0,
                   help="Start-UA voor feedforward [W/K] (wordt online bijgeleerd)")
    f.add_argument("--ff-ki",          type=float, default=0.003,
                   help="Integraalversterkingsfactor voor kamerfout-correctie")
    f.add_argument("--ff-leersnelheid", type=float, default=0.002,
                   help="Leersnelheid voor UA_huis (0=geen leren, 0.01=snel)")
    f.add_argument("--ff-coast-k",     type=float, default=1.5,
                   help="Anticipatiezone: stand begint af te bouwen op deze °C "
                        "voor setpoint (groter = eerder afremmen)")
    p.add_argument("--optimize", action="store_true",
                   help="Optimaliseer Kp/Ki/Kd via scipy differential_evolution "
                        "en vergelijk met historische data uit CSV")
    return p.parse_args()


# ── CSV laden ──────────────────────────────────────────────────────────────
def load_csv(path, start=None, end=None):
    df_raw = pd.read_csv(path, parse_dates=["last_changed"])

    if "current_temperature" in df_raw.columns:
        print("Formaat v2 gedetecteerd (history(2).csv — climate + opentherm)")
        df_raw = _preprocess_history2(df_raw)
        entity_map_use = ENTITY_MAP_V2
        ref_map_use    = REFERENCE_MAP_V2
        obs_map_use    = OBSERVE_MAP_V2
    else:
        entity_map_use = ENTITY_MAP
        ref_map_use    = REFERENCE_MAP
        obs_map_use    = OBSERVE_MAP

    all_entities = {**entity_map_use, **ref_map_use, **obs_map_use}
    df = df_raw[df_raw["entity_id"].isin(all_entities.keys())].copy()
    df["state"] = pd.to_numeric(df["state"], errors="coerce")
    df = df.dropna(subset=["state"])
    df["ts"] = df["last_changed"].dt.floor("h")

    if start:
        df = df[df["ts"] >= pd.Timestamp(start, tz="UTC")]
    if end:
        df = df[df["ts"] <  pd.Timestamp(end,   tz="UTC")]

    def make_pivot(entity_map):
        sub = df[df["entity_id"].isin(entity_map.keys())]
        if sub.empty:
            return pd.DataFrame()
        p = sub.pivot_table(index="ts", columns="entity_id",
                            values="state", aggfunc="mean")
        return p.sort_index().rename(columns=entity_map)

    pivot   = make_pivot(entity_map_use)
    ref     = make_pivot(ref_map_use)
    observe = make_pivot(obs_map_use)

    if pivot.empty:
        return pivot, ref, observe

    print(f"CSV geladen: {len(pivot)} uur-stappen  "
          f"({pivot.index[0]}  →  {pivot.index[-1]})")

    pivot = pivot.resample("10min").interpolate(method="linear")
    if not ref.empty:
        ref = ref.resample("10min").interpolate(method="linear")
    if not observe.empty:
        observe = observe.resample("10min").interpolate(method="linear")

    print(f"  → geïnterpoleerd naar 10-min stappen: {len(pivot)} tijdstappen")
    print(f"  Buitentemperatuur:      "
          f"{'ja' if 'chofu/sim/outside' in pivot.columns else 'ONTBREEKT'}")
    print(f"  Kamer setpoint (auto):  "
          f"{'ja (chofu/cmd/kamer_setpoint)' if 'chofu/cmd/kamer_setpoint' in pivot.columns else 'niet in CSV → gebruik --kamer-setpoint'}")
    print(f"  Water setpoint (water): "
          f"{'ja' if 'chofu/sim/water_setpoint' in pivot.columns else 'niet in CSV → stooklijn of --water-setpoint'}")
    if not ref.empty:
        print(f"  Historische referentie: {list(ref.columns)}")
    return pivot, ref, observe


# ── Hoofd replay loop ──────────────────────────────────────────────────────
def run_replay(pivot, model, controller, modus="auto", kamer_sp=20.0,
               water_sp=None, verbose=True):
    """
    Gesloten regelkring: controller bepaalt stand → HouseModel berekent
    nieuwe temperaturen → volgende stap. Zo snel als Python rekent.

    controller: ArduinoPID of FeedforwardController instantie.
    verbose=False onderdrukt alle print-uitvoer (gebruik bij optimalisatie).
    Retourneert pd.DataFrame met simulatieresultaten.
    """
    model.reset()
    timestamps = pivot.index.tolist()
    n = len(timestamps)

    if n > 1:
        diffs = pd.Series(timestamps).diff().dropna().dt.total_seconds()
        median_interval_s = diffs.median()
    else:
        median_interval_s = 3600.0

    ctrl_name = type(controller).__name__
    if verbose:
        print(f"\nHuismodel:")
        print(f"  UA_house   = {model.UA_house:.0f} W/K  "
              f"→  ontwerplast @ -10°C: {model.UA_house * 30 / 1000:.1f} kW")
        print(f"  C_th       = {model.C_th / 1e6:.2f} MJ/K  "
              f"→  tijdconstante τ = {model.C_th / model.UA_house / 3600:.0f} uur")
        print(f"  UA_emitter = {model.UA_emitter:.0f} W/K  "
              f"→  vermogen @ 35°C aanvoer/20°C kamer: "
              f"{model.UA_emitter * (32.5 - 20) / 1000:.1f} kW")
        print(f"  C_water    = {model.C_water / 1e3:.0f} kJ/K  "
              f"(≈{model.C_water / 4190:.0f} L water)")
        print(f"  COP_eta    = {model.COP_eta:.2f}  "
              f"→  COP @ 35°C / 0°C buiten ≈ {model.COP_eta * (308.15 / 35):.1f}")
        if isinstance(controller, ArduinoPID):
            print(f"\n{ctrl_name}: Kp={controller.Kp}  Ki={controller.Ki}"
                  f"  Kd={controller.Kd}"
                  f"  corr={controller.kamer_corr_normaal}/{controller.kamer_corr_groot}")
        else:
            print(f"\n{ctrl_name}: UA_start={controller.UA_house:.0f} W/K"
                  f"  Ki={controller.Ki_kamer}  leersnelheid={controller.ua_learn_rate}")
        print(f"  hyst_slow={HYST_SLOW_PROD//1000}s  "
              f"hyst_fast={HYST_FAST_PROD//1000}s")
        print(f"\nReplay: {n} stappen × {median_interval_s:.0f}s  "
              f"= {n * median_interval_s / 3600:.1f} uur gesimuleerde tijd\n")

    sim_records = []

    try:
        for i, ts in enumerate(timestamps):
            row = pivot.loc[ts]

            dt_s = float((ts - timestamps[i - 1]).total_seconds()) if i > 0 \
                   else median_interval_s

            t_outside = row.get("chofu/sim/outside")
            if t_outside is None or pd.isna(t_outside):
                t_outside = model.t_kamer - 5.0

            if modus == "auto":
                sp_csv = row.get("chofu/cmd/kamer_setpoint")
                sp = sp_csv if (sp_csv is not None and not pd.isna(sp_csv)) else kamer_sp
                t_water_gewenst = 40.0
            else:  # water
                stooklijn = min(45.0, 28.0 + max(0.0, (15.0 - t_outside) * 0.68))
                if water_sp is not None:
                    # Vaste waarde opgegeven via CLI
                    wsp = water_sp
                else:
                    # Altijd eigen stooklijn gebruiken — de historische Opentherm-setpoint
                    # was van een andere regelaar en is niet geschikt als invoer
                    wsp = stooklijn
                sp = kamer_sp
                t_water_gewenst = wsp

            stand = controller.step(
                dt_s, t_outside,
                model.t_supply, model.t_return,
                model.t_kamer, sp, t_water_gewenst,
                p_to_house=model.p_to_house,
            )

            delta_t = model.t_supply - model.t_return
            model.step(t_outside, stand, dt_s)

            if modus == "water":
                ua_est = (getattr(controller, "UA_emitter_est", None)
                          or getattr(controller, "UA_emitter_eff", 171.0))
            else:
                ua_est = (getattr(controller, "UA_house_est", None)
                          or getattr(controller, "UA_house", None))
            sim_records.append({
                "sim_time":             ts,
                "chofu/outside":        t_outside,
                "chofu/supply":         model.t_supply,
                "chofu/return":         model.t_return,
                "chofu/kamer":          model.t_kamer,
                "chofu/kamer_gewenst":  sp,
                "chofu/stand":          stand,
                "chofu/aan":            int(controller.wp_aan),
                "chofu/pid":            controller.pid_output,
                "chofu/doel_setpoint":  controller.doel_setpoint,
                "chofu/delta_t":        delta_t,
                "chofu/vermogen":       VERMOGEN[stand],
                "chofu/water_setpoint": t_water_gewenst,
                "chofu/UA_est":         ua_est,
            })

            if verbose:
                pct     = (i + 1) / n * 100
                cop_str = f"COP={model.cop:.1f}" if stand > 0 else "WP=uit"
                print(f"[{pct:5.1f}%] {ts.strftime('%Y-%m-%d %H:%M')}  "
                      f"buiten={t_outside:.1f}  aanvoer={model.t_supply:.1f}  "
                      f"kamer={model.t_kamer:.1f}  sp={sp:.1f}  "
                      f"[St={stand} {cop_str}  Php={model.p_to_house/1000:.1f}kW]")

    except KeyboardInterrupt:
        if verbose:
            print("\n\nGestopt door gebruiker.")

    if not sim_records:
        return pd.DataFrame()
    return pd.DataFrame(sim_records).set_index("sim_time")


# ── PID optimalisatie ──────────────────────────────────────────────────────
def optimize_pid(pivot, ref, model, pid_kw_base, modus,
                 kamer_sp=20.0, water_sp=None):
    """
    Zoek Kp, Ki, Kd die de RMSE minimaliseren tussen gesimuleerde temperatuur
    en historische meting:
      water modus → chofu/supply  vs  historisch/aanvoer
      auto  modus → chofu/kamer   vs  historisch/kamer

    pid_kw_base: dict met vaste ArduinoPID-parameters (corr, hyst, interval).
    """
    try:
        import numpy as np
        from scipy.optimize import differential_evolution
    except ImportError:
        print("scipy niet gevonden — pip install scipy numpy")
        return None

    ref_col = "historisch/aanvoer" if modus == "water" else "historisch/kamer"
    sim_col = "chofu/supply"       if modus == "water" else "chofu/kamer"

    if ref_col not in ref.columns:
        print(f"Referentiekolom '{ref_col}' ontbreekt in CSV.")
        return None

    ref_series = ref[[ref_col]].dropna()
    n_eval = [0]
    best   = [1e9]

    def objective(params):
        Kp, Ki, Kd = params
        ctrl = ArduinoPID(**{**pid_kw_base, "Kp": Kp, "Ki": Ki, "Kd": Kd})
        sim_df = run_replay(pivot, model, ctrl,
                            modus=modus, kamer_sp=kamer_sp, water_sp=water_sp,
                            verbose=False)
        n_eval[0] += 1
        if sim_col not in sim_df.columns:
            return 1e6
        merged = sim_df[[sim_col]].join(ref_series, how="inner").dropna()
        if len(merged) < 10:
            return 1e6
        rmse = float(np.sqrt(((merged[sim_col] - merged[ref_col]) ** 2).mean()))
        if rmse < best[0]:
            best[0] = rmse
            print(f"  [{n_eval[0]:4d}] Kp={Kp:.3f} Ki={Ki:.5f} Kd={Kd:.3f}"
                  f"  → RMSE={rmse:.3f}°C  ★")
        elif n_eval[0] % 50 == 0:
            print(f"  [{n_eval[0]:4d}] beste RMSE tot nu: {best[0]:.3f}°C")
        return rmse

    bounds = [(0.1, 20.0), (0.001, 0.3), (0.0, 1.5)]
    print(f"\nOptimalisatie {modus}-modus  ({sim_col} vs {ref_col})")
    print(f"Zoekruimte: Kp=[0.1,20]  Ki=[0.001,0.3]  Kd=[0,1.5]")
    print(f"Algoritme: differential_evolution  popsize=12  maxiter=60\n")

    result = differential_evolution(
        objective, bounds,
        maxiter=60, popsize=12, tol=0.005,
        seed=42, polish=True, workers=1,
    )

    Kp_opt, Ki_opt, Kd_opt = result.x
    print(f"\nResultaat ({modus}):")
    print(f"  Kp = {Kp_opt:.4f}  Ki = {Ki_opt:.5f}  Kd = {Kd_opt:.4f}")
    print(f"  RMSE = {result.fun:.3f}°C  ({n_eval[0]} evaluaties)")
    print(f"\nGebruik:")
    print(f"  python replay_simulation.py --modus {modus}"
          f" --kp {Kp_opt:.3f} --ki {Ki_opt:.5f} --kd {Kd_opt:.3f}")
    return result.x, result.fun


# ── Resultaten opslaan ─────────────────────────────────────────────────────
def save_results(results_df, ref, observe, output_dir, modus="auto"):
    os.makedirs(output_dir, exist_ok=True)
    ts_str = datetime.now().strftime("%Y%m%d_%H%M%S")

    frames   = [df for df in [results_df, ref, observe] if not df.empty]
    combined = pd.concat(frames, axis=1) if len(frames) > 1 else results_df
    csv_path = os.path.join(output_dir, f"sim_results_{ts_str}.csv")
    combined.to_csv(csv_path)
    print(f"\nResultaten CSV: {csv_path}  "
          f"({len(combined)} rijen, {len(combined.columns)} kolommen)")

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import matplotlib.dates as mdates

        def fmt_ax(ax):
            ax.xaxis.set_major_formatter(mdates.DateFormatter("%d/%m"))
            ax.xaxis.set_major_locator(mdates.AutoDateLocator())
            ax.tick_params(axis="x", rotation=30, labelsize=7)
            ax.grid(True, alpha=0.3)
            ax.legend(fontsize=8)

        idx = results_df.index

        # ── Figuur 1: Temperaturen ─────────────────────────────────────────
        fig, axes = plt.subplots(4, 1, figsize=(14, 14), sharex=True)
        fig.suptitle(f"Chofu WP — Temperaturen  [{modus} modus]", fontsize=11)

        # Subplot 1: aanvoer / retour / doel setpoint
        ax = axes[0]
        for col, lbl, clr, ls in [
            ("chofu/supply",        "Aanvoer (model)",  "#e74c3c", "-"),
            ("chofu/return",        "Retour (model)",   "#e67e22", "-"),
            ("chofu/doel_setpoint", "Doel setpoint",    "#3498db", "-"),
        ]:
            if col in results_df:
                ax.plot(idx, results_df[col], label=lbl, color=clr,
                        lw=1.2, linestyle=ls)
        if not ref.empty and "historisch/aanvoer" in ref:
            ax.plot(ref.index, ref["historisch/aanvoer"],
                    label="Aanvoer hist.", color="#e74c3c",
                    lw=1, linestyle="--", alpha=0.55)
        ax.set_ylabel("°C")
        ax.set_title("Aanvoer / Retour")
        fmt_ax(ax)

        # Subplot 2: water setpoint (water modus) of kamer gewenst (auto modus)
        ax = axes[1]
        if modus == "water":
            if "chofu/water_setpoint" in results_df:
                ax.plot(idx, results_df["chofu/water_setpoint"],
                        label="Water setpoint (gevraagd)", color="#3498db", lw=1.2)
            if not ref.empty and "historisch/water_setpoint" in ref:
                ax.plot(ref.index, ref["historisch/water_setpoint"],
                        label="Water setpoint hist.", color="#3498db",
                        lw=1, linestyle="--", alpha=0.55)
            ax.set_title("Water setpoint (gevraagd)")
        else:
            if "chofu/kamer_gewenst" in results_df:
                ax.plot(idx, results_df["chofu/kamer_gewenst"],
                        label="Kamer setpoint (gewenst)", color="#8e44ad", lw=1.2)
            ax.set_title("Kamer setpoint (gewenst)")
        ax.set_ylabel("°C")
        fmt_ax(ax)

        # Subplot 3: kamer temperatuur (model + historisch)
        ax = axes[2]
        if "chofu/kamer" in results_df:
            ax.plot(idx, results_df["chofu/kamer"],
                    label="Kamer (model)", color="#2980b9", lw=1.2)
        if not ref.empty and "historisch/kamer" in ref:
            ax.plot(ref.index, ref["historisch/kamer"],
                    label="Kamer hist.", color="#2980b9",
                    lw=1, linestyle="--", alpha=0.55)
        ax.set_ylabel("°C")
        ax.set_title("Kamer temperatuur (gemodelleerd)")
        fmt_ax(ax)

        # Subplot 4: buiten + delta T
        ax = axes[3]
        if "chofu/outside" in results_df:
            ax.plot(idx, results_df["chofu/outside"],
                    label="Buiten", color="#27ae60", lw=1.2)
        if "chofu/delta_t" in results_df:
            ax.plot(idx, results_df["chofu/delta_t"],
                    label="Delta T", color="#95a5a6", lw=1.2)
        ax.set_ylabel("°C")
        ax.set_title("Buiten / Delta T")
        fmt_ax(ax)

        fig.tight_layout()
        p = os.path.join(output_dir, f"sim_temperaturen_{ts_str}.png")
        fig.savefig(p, dpi=150)
        plt.close(fig)
        print(f"Grafiek: {p}")

        # ── Figuur 2: Regeling ─────────────────────────────────────────────
        fig, axes = plt.subplots(3, 1, figsize=(14, 9), sharex=True)
        fig.suptitle(f"Chofu WP — Regeling  [{modus} modus]", fontsize=11)

        ax = axes[0]
        if "chofu/stand" in results_df:
            ax.fill_between(idx, results_df["chofu/stand"],
                            step="post", alpha=0.4, color="#e74c3c", label="Stand")
            ax.plot(idx, results_df["chofu/stand"],
                    drawstyle="steps-post", color="#e74c3c", lw=1)
        ax.set_ylabel("Stand (0–8)")
        ax.set_ylim(-0.2, 9)
        ax.set_title("Compressorstand")
        fmt_ax(ax)

        ax = axes[1]
        if "chofu/pid" in results_df:
            ax.plot(idx, results_df["chofu/pid"],
                    color="#3498db", lw=1.2, label="PID %")
        ax.set_ylabel("PID (%)")
        ax.set_ylim(-5, 105)
        fmt_ax(ax)

        ax = axes[2]
        if "chofu/aan" in results_df:
            ax.fill_between(idx, results_df["chofu/aan"],
                            step="post", alpha=0.5, color="#27ae60", label="WP aan")
        ax.set_yticks([0, 1])
        ax.set_yticklabels(["UIT", "AAN"])
        fmt_ax(ax)

        fig.tight_layout()
        p = os.path.join(output_dir, f"sim_regeling_{ts_str}.png")
        fig.savefig(p, dpi=150)
        plt.close(fig)
        print(f"Grafiek: {p}")

        # ── Figuur 3: Vermogen ─────────────────────────────────────────────
        fig, axes = plt.subplots(2, 1, figsize=(14, 7), sharex=True)
        fig.suptitle(f"Chofu WP — Vermogen  [{modus} modus]", fontsize=11)

        ax = axes[0]
        if "chofu/vermogen" in results_df:
            ax.fill_between(idx, results_df["chofu/vermogen"],
                            alpha=0.4, color="#e74c3c", label="Gesimuleerd (W)")
            ax.plot(idx, results_df["chofu/vermogen"], color="#e74c3c", lw=1)
        if not observe.empty and "historisch/vermogen_w" in observe:
            ax.plot(observe.index, observe["historisch/vermogen_w"],
                    color="#7f8c8d", lw=1.2, linestyle="--",
                    label="Historisch (W)")
        ax.set_ylabel("Vermogen (W)")
        fmt_ax(ax)

        ax = axes[1]
        if "chofu/comp_hz" in results_df:
            ax.plot(idx, results_df["chofu/comp_hz"],
                    color="#8e44ad", lw=1.2, label="Comp Hz")
        if "chofu/pomp" in results_df:
            ax.plot(idx, results_df["chofu/pomp"],
                    color="#2980b9", lw=1, linestyle="--", label="Pomp %")
        ax.set_ylabel("Hz / %")
        fmt_ax(ax)

        fig.tight_layout()
        p = os.path.join(output_dir, f"sim_vermogen_{ts_str}.png")
        fig.savefig(p, dpi=150)
        plt.close(fig)
        print(f"Grafiek: {p}")

    except ImportError:
        print("matplotlib niet gevonden — grafieken overgeslagen "
              "(pip install matplotlib)")


# ── Main ───────────────────────────────────────────────────────────────────
def main():
    args = parse_args()

    pivot, ref, observe = load_csv(args.csv, args.start, args.end)
    if pivot.empty:
        print("Geen data na filters. Stoppen.")
        sys.exit(1)

    model = HouseModel(
        UA_house      = args.ua,
        C_th          = args.cth,
        UA_emitter    = args.ua_emitter,
        C_water       = args.cwater,
        COP_eta       = args.cop_eta,
        t_kamer_init  = args.t_init,
        t_supply_init = 35.0,
    )

    # Vaste PID-constructorparameters (niet door optimizer gewijzigd)
    pid_kw_base = dict(
        modus              = args.modus,
        kamer_corr_normaal = args.kamer_corr_normaal,
        kamer_corr_groot   = args.kamer_corr_groot,
        max_stand_stap     = args.max_stand_stap,
        hyst_slow_s        = HYST_SLOW_PROD / 1000,
        hyst_fast_s        = HYST_FAST_PROD / 1000,
        hyst_down_s        = HYST_DOWN_PROD / 1000,
        pid_interval_s     = PID_INT_PROD   / 1000,
    )

    run_kw = dict(
        modus    = args.modus,
        kamer_sp = args.kamer_setpoint,
        water_sp = args.water_setpoint,
    )

    Kp, Ki, Kd = args.kp, args.ki, args.kd

    if args.optimize:
        result = optimize_pid(pivot, ref, model, pid_kw_base,
                              kamer_sp=args.kamer_setpoint,
                              water_sp=args.water_setpoint,
                              modus=args.modus)
        if result is None:
            sys.exit(1)
        Kp, Ki, Kd = result[0]
        print("\nSimulatie met optimale parameters...\n")

    if args.controller == "ff":
        # UA_emitter_eff = effectieve UA bij aanvoertemperatuur, afgeleid van modelparameters
        # 2·G·UA_emitter / (2·G + UA_emitter)  met G = 600 W/K
        G_FLOW = 600.0
        ua_emitter_eff = (2.0 * G_FLOW * args.ua_emitter
                          / (2.0 * G_FLOW + args.ua_emitter))
        controller = FeedforwardController(
            modus           = args.modus,
            UA_house        = args.ff_ua,
            Ki_kamer        = args.ff_ki,
            ua_learn_rate   = args.ff_leersnelheid,
            kamer_coast_k   = args.ff_coast_k,
            max_stand_stap  = args.max_stand_stap,
            hyst_slow_s     = HYST_SLOW_PROD / 1000,
            hyst_fast_s     = HYST_FAST_PROD / 1000,
            hyst_down_s     = HYST_DOWN_PROD / 1000,
            UA_emitter_eff  = ua_emitter_eff,
        )
    else:
        controller = ArduinoPID(**pid_kw_base, Kp=Kp, Ki=Ki, Kd=Kd)

    sim_df = run_replay(pivot, model, controller, **run_kw)

    if not sim_df.empty:
        print(f"\n{len(sim_df)} stappen gesimuleerd.")
        save_results(sim_df, ref, observe, args.output, modus=args.modus)
        print(f"\nOpgeslagen in: {os.path.abspath(args.output)}/")
    else:
        print("\nGeen simulatieresultaten.")


if __name__ == "__main__":
    main()
