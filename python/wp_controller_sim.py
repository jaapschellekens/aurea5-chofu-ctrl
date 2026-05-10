#!/usr/bin/env python3
"""
wp_controller_sim.py — Pure Python controller + huis simulatie

Simuleert ZOWEL de Arduino-regelaar ALS het huis/WP-model volledig in Python,
zonder MQTT of hardware. Identiek aan de firmware-logica in regelaar.cpp.

Gebruik:
    python wp_controller_sim.py --modus ff_auto --days 5 --outside 5 --swing 5
    python wp_controller_sim.py --modus auto    --days 3 --outside 3 --csv out.csv
    python wp_controller_sim.py --modus ff_water --days 7 --outside 2 --kp 19.9
    python wp_controller_sim.py --optimise --modus ff_auto  # parameter sweep

Optimalisatie (grid search op Kp/Ki/Kd of UA-waarden):
    python wp_controller_sim.py --optimise --modus auto
"""
import argparse
import csv
import math
import sys
from dataclasses import dataclass, field
from typing import Optional

# ═══════════════════════════════════════════════════════════════════════════════
#  VERMOGENSTABEL (identiek aan firmware types.h)
# ═══════════════════════════════════════════════════════════════════════════════
VERMOGEN = [0, 240, 420, 640, 850, 1050, 1250, 1450, 1550, 1650, 1700, 1750, 1800]

# ═══════════════════════════════════════════════════════════════════════════════
#  CONTROLLER PARAMETERS (spiegelt globals.cpp + types.h)
# ═══════════════════════════════════════════════════════════════════════════════
@dataclass
class Params:
    # PID auto modus (aanvoer vs stooklijn setpoint) — geoptimaliseerd KGE
    Kp: float = 75.0    # was 19.9
    Ki: float = 0.800   # was 0.084
    Kd: float = 0.010   # was 0.036
    # PID water modus (aanvoer vs variabele stooklijn) — geoptimaliseerd KGE
    Kp_water: float = 50.0
    Ki_water: float = 0.800
    Kd_water: float = 0.010
    pid_interval_s: float = 5.0          # in Python: seconden (firmware: 5000 ms)

    # Hysteresis (seconden; firmware: ms)
    hyst_slow_s: float = 600.0
    hyst_fast_s: float = 120.0
    hyst_down_s: float = 60.0             # water mode: snel terugschakelen
    auto_hyst_down_s: float = 300.0       # auto mode: trager terugschakelen — minder oscillatie

    # AUTO stabiliteitsparameters
    auto_aanvoer_deadband: float = 0.5  # °C — geen standwijziging binnen deadband
    auto_max_stap: int = 1              # max standwijziging per cycle (±1)

    # Feedforward
    ff_ua_house:   float = 272.5         # W/K
    ff_ua_emitter: float = 250.0         # W/K
    ff_ki_auto:    float = 0.026
    ff_ki_water:   float = 0.017
    ff_coast_auto: float = 0.30          # °C
    ff_coast_water: float = 2.5          # °C
    ff_afschakel_auto:  float = -0.5     # °C fout waarbij FF terugschakelt (auto)
    ff_learn_rate: float = 0.0002

    # Stooklijn
    stooklijn_sp:       float = 28.0     # basis aanvoer setpoint
    stooklijn_grens:    float = 15.0     # °C buiten → begin boost
    stooklijn_factor:   float = 0.68
    stooklijn_uit:      float = 18.0     # °C buiten → WP uit (ff_auto) — huis heeft bij 18°C nog ~500W warmtevraag
    stooklijn_aan:      float = 16.0     # °C buiten → WP weer aan na buitenseizoen (2°C hysteresis)

    # FF_AUTO start-beperking
    ff_min_off_s:       float = 600.0    # min uitschakelperiode na seizoensstop
    ff_thermal_min_off_s: float = 60.0  # min uitschakelperiode na thermische stop (overshoot)
    ff_restart_coast:   float = 0.20     # °C onder setpoint vereist voor herstart vanuit stand 0
    ff_afschakel_lookahead_s: float = 300.0  # sec vooruitkijken voor predictieve terugschakeling
    ff_afschakel_deriv_window_s: float = 1200.0  # meetvenster voor afgeleide (20 min past bij τ_huis≈13h)

    # Drempels
    supply_max:        float = 60.0
    t_vorst:           float = 2.0
    auto_aan_drempel:  float = 0.4       # kamer °C onder setpoint → WP aan
    auto_uit_drempel:  float = -0.4      # kamer °C boven setpoint → afbouwen

    # Setpoints
    t_kamer_gewenst:   float = 20.0
    t_water_gewenst:   float = 32.0

    # Koeling
    koeling_modus:     bool  = False
    supply_min:        float = 17.0   # condensatiebescherming aanvoer (°C)
    koeling_afschakel: float = 0.5    # °C ónder setpoint → koeling terugschakelen
    koeling_min_buiten: float = 18.0  # min buitentemp voor koeling actief

    # FF leer-aan/uit
    learning_enabled:  bool  = True


# ═══════════════════════════════════════════════════════════════════════════════
#  CONTROLLER TOESTAND (spiegelt ControllerState struct)
# ═══════════════════════════════════════════════════════════════════════════════
@dataclass
class ControllerState:
    stand: int   = 0
    wp_aan: bool = False
    pid_integraal: float = 0.0
    pid_vorige_fout: float = 0.0
    pid_output: float = 0.0
    ff_integraal: float = 0.0
    vorige_stand_wijz_s: float = -9999.0   # ver in het verleden
    startup_s: float = -9999.0             # wanneer WP voor het laatste aansloeg
    wp_uit_s: float = -9999.0             # wanneer WP voor het laatste uitschakelde
    wp_thermal_stop: bool = False         # True = laatste stop was thermisch (overshoot), False = seizoensmatig

    def zet_uit(self):
        self.stand = 0
        self.wp_aan = False
        self.pid_integraal = 0.0
        self.pid_vorige_fout = 0.0
        self.pid_output = 0.0
        self.ff_integraal = 0.0

    def reset_pid(self):
        # Zacht reset: halveer integraal zodat herstart minder overreageert
        self.pid_integraal   *= 0.5
        self.pid_vorige_fout  = 0.0

    def reset_ff(self):
        self.ff_integraal = 0.0


# ═══════════════════════════════════════════════════════════════════════════════
#  HUIS + WP MODEL (identiek aan wp_simulator.py HouseModel)
# ═══════════════════════════════════════════════════════════════════════════════
UA_HOUSE_SIM   = 263.0    # W/K  werkelijk warmteverlies huis
C_TH           = 12.5e6   # J/K  thermische massa huis
UA_EMITTER_SIM = 344.0    # W/K  warmteafgifte emitters
C_WATER        = 293e3    # J/K  waterinhoud circuit
COP_ETA        = 0.40
G_FLOW         = 600.0    # W/K  m_dot × Cp

@dataclass
class HouseModel:
    t_kamer:  float = 20.0
    t_supply: float = 32.0

    def __post_init__(self):
        self._ua_eff = 2.0 * G_FLOW * UA_EMITTER_SIM / (2.0 * G_FLOW + UA_EMITTER_SIM)

    def cop(self, t_supply: float, t_outside: float) -> float:
        T_s = t_supply  + 273.15
        T_o = t_outside + 273.15
        dT  = T_s - T_o
        if dT < 1.0:
            return 5.0
        return min(COP_ETA * T_s / dT, 7.0)

    def cop_koel(self, t_supply: float, t_outside: float) -> float:
        """COP koeling: warmte onttrokken aan water / elektrisch vermogen."""
        T_s = t_supply  + 273.15
        T_o = t_outside + 273.15
        dT  = T_o - T_s          # buiten warmer dan koude aanvoer
        if dT < 1.0:
            return 1.0
        return min(COP_ETA * T_s / dT, 7.0)

    def step(self, stand: int, t_outside: float, dt_s: float,
             koeling: bool = False, supply_min: float = 17.0):
        p_elec = float(VERMOGEN[max(0, min(stand, 12))])
        if koeling:
            # Koelfysica: WP onttrekt warmte aan water, kamer geeft warmte af aan koud water,
            # buiten (warm) geeft warmte aan huis.
            cop    = self.cop_koel(self.t_supply, t_outside)
            p_cool = p_elec * cop                                              # W uit water gehaald
            p_emit = self._ua_eff * max(0.0, self.t_kamer - self.t_supply)    # kamer → koud water
            p_loss = UA_HOUSE_SIM * (self.t_kamer - t_outside)                # negatief in zomer = warmte-inval
            self.t_supply += (-p_cool + p_emit) / C_WATER * dt_s
            self.t_supply  = max(self.t_supply, supply_min)                    # condensatiebescherming
            self.t_kamer  += (-p_emit - p_loss) / C_TH    * dt_s
            t_return = self.t_supply + p_emit / G_FLOW if G_FLOW > 0 else self.t_supply
            return self.t_supply, t_return, self.t_kamer, cop, p_cool
        else:
            cop    = self.cop(self.t_supply, t_outside)
            p_heat = p_elec * cop
            p_emit = self._ua_eff * max(0.0, self.t_supply - self.t_kamer)
            p_loss = UA_HOUSE_SIM * (self.t_kamer - t_outside)
            self.t_supply += (p_heat - p_emit) / C_WATER * dt_s
            self.t_kamer  += (p_emit - p_loss) / C_TH    * dt_s
            t_return = self.t_supply - p_emit / G_FLOW if G_FLOW > 0 else self.t_supply
            return self.t_supply, t_return, self.t_kamer, cop, p_heat


# ═══════════════════════════════════════════════════════════════════════════════
#  ARDUINO CONTROLLER (spiegelt regelaar.cpp)
# ═══════════════════════════════════════════════════════════════════════════════
class Controller:
    def __init__(self, params: Params):
        self.p    = params
        self.ctrl = ControllerState()
        # lerende UA-waarden (kunnen tijdens simulatie drift)
        self.ff_ua_house   = params.ff_ua_house
        self.ff_ua_emitter = params.ff_ua_emitter
        self._pid_timer_s  = 0.0     # accumuleer tijd tot volgende PID-cyclus
        self.doel_setpoint = params.stooklijn_sp
        # Ring-buffer voor de kamertemperatuurgeschiedenis (sliding-window afgeleide)
        self._kamer_history: list[tuple[float,float]] = []  # (now_s, t_kamer)

    # ── Hulpfuncties ─────────────────────────────────────────────────────────
    def _ff_cop(self, t_aanvoer: float, t_buiten: float) -> float:
        T_s = t_aanvoer + 273.15
        T_o = t_buiten  + 273.15
        dT  = T_s - T_o
        if dT < 1.0:
            return 1.0
        return max(1.0, min(T_s / dT * 0.40, 6.0))

    def _ff_cop_koel(self, t_aanvoer: float, t_buiten: float) -> float:
        """COP voor koelmodus: buiten warmer dan aanvoer."""
        T_s = t_aanvoer + 273.15
        T_o = t_buiten  + 273.15
        dT  = T_o - T_s
        if dT < 1.0:
            return 1.0
        return max(1.0, min(T_s / dT * 0.40, 6.0))

    def _ff_stand_voor_vermogen(self, p_elec: float) -> int:
        for s in range(13):
            if VERMOGEN[s] >= p_elec:
                return min(s, 8)
        return 8

    def _stooklijn(self, t_outside: float) -> float:
        p = self.p
        if t_outside < p.stooklijn_grens:
            return min(45.0, p.stooklijn_sp + (p.stooklijn_grens - t_outside) * p.stooklijn_factor)
        return p.stooklijn_sp

    # ── Feedforward regelaar (ff_auto + ff_water) ────────────────────────────
    def _pas_ff_aan(self, t_supply: float, t_kamer: float, t_outside: float, now_s: float):
        p    = self.p
        ctrl = self.ctrl
        is_water  = self._modus == "ff_water"
        is_koeling = p.koeling_modus

        # Koeling: stuur door naar koelmethode
        if is_koeling:
            self._pas_ff_koel_aan(t_supply, t_kamer, t_outside, now_s, is_water)
            return

        stooklijn = self._stooklijn(t_outside)
        # ff_water: in operationeel systeem stuurt Adam de stooklijn als water setpoint via MQTT
        # ff_auto:  stooklijn is de aanvoer-doeltemperatuur
        wsp = stooklijn
        self.doel_setpoint = wsp

        # Buiten seizoen (alleen ff_auto) — [A] hysteresis: uit bij stooklijn_uit, aan bij stooklijn_aan
        if not is_water:
            if t_outside > p.stooklijn_uit:
                if ctrl.wp_aan:
                    ctrl.wp_uit_s = now_s
                    ctrl.wp_thermal_stop = False  # seizoensstop
                    ctrl.zet_uit()
                return
            # Alleen verder als t_outside ≤ stooklijn_aan (hysteresis), of WP al aan was, of laatste stop was thermisch
            if not ctrl.wp_aan and t_outside > p.stooklijn_aan and not ctrl.wp_thermal_stop:
                return

        regel_fout = (wsp - t_supply) if is_water else (p.t_kamer_gewenst - t_kamer)
        afschakeldrempel = -1.5 if is_water else p.ff_afschakel_auto

        # Predictieve terugschakeling (alleen ff_auto): extrapoleer kamer over lookahead_s.
        # Afgeleide bepaald over een lang meetvenster (deriv_window_s) passend bij τ_huis≈13h.
        if not is_water and p.ff_afschakel_lookahead_s > 0:
            self._kamer_history.append((now_s, t_kamer))
            # Verwijder te oude metingen (ouder dan deriv_window_s)
            cutoff = now_s - p.ff_afschakel_deriv_window_s
            while len(self._kamer_history) > 1 and self._kamer_history[0][0] < cutoff:
                self._kamer_history.pop(0)
            # Afgeleide: helling over het volledige resterende venster (oudste → nieuwste meting)
            if len(self._kamer_history) >= 2:
                t0, k0 = self._kamer_history[0]
                dt_win = now_s - t0
                kamer_dt = (t_kamer - k0) / dt_win if dt_win > 0 else 0.0
            else:
                kamer_dt = 0.0
            kamer_predicted = t_kamer + kamer_dt * p.ff_afschakel_lookahead_s
            regel_fout_eff  = p.t_kamer_gewenst - kamer_predicted
        else:
            regel_fout_eff = regel_fout

        # Te warm: terugregelen (gebruik predictieve fout zodat bijtijds teruggeschakeld wordt)
        if regel_fout_eff < afschakeldrempel:
            if t_outside < p.t_vorst:
                ctrl.stand = 1; ctrl.wp_aan = True; ctrl.vorige_stand_wijz_s = now_s
            elif ctrl.stand > 0 and (now_s - ctrl.vorige_stand_wijz_s) >= p.hyst_down_s:
                stap = 2 if (-regel_fout > 4.0) else 1
                ctrl.stand = max(0, ctrl.stand - stap)
                ctrl.vorige_stand_wijz_s = now_s
                ctrl.wp_aan = ctrl.stand > 0
                if ctrl.stand == 0:
                    ctrl.wp_uit_s = now_s
                    ctrl.wp_thermal_stop = True   # thermische stop
                    ctrl.reset_ff()
            return

        # [B+C] Herstart vanuit stand 0: minimum uitschakelperiode + hogere drempel
        if not is_water and ctrl.stand == 0:
            # Kortere wachttijd na thermische stop (overshoot), langere na seizoensstop
            min_off = p.ff_thermal_min_off_s if ctrl.wp_thermal_stop else p.ff_min_off_s
            min_off_ok = (now_s - ctrl.wp_uit_s) >= min_off
            drempel_ok = regel_fout > p.ff_restart_coast
            if not (min_off_ok and drempel_ok):
                return  # nog niet herstarten

        # FF: benodigde vermogen
        if is_water:
            p_nodig = max(0.0, self.ff_ua_emitter * (wsp - t_kamer))
            cop     = self._ff_cop(max(wsp, t_kamer + 1.0), t_outside)
        else:
            p_nodig = max(0.0, self.ff_ua_house * (p.t_kamer_gewenst - t_outside))
            cop     = self._ff_cop(t_supply, t_outside)

        stand_ff = self._ff_stand_voor_vermogen(p_nodig / max(cop, 1.0))

        coast_k = p.ff_coast_water if is_water else p.ff_coast_auto
        if regel_fout > coast_k:
            stand_ff = min(8, stand_ff + 1)

        # Integraalcorrectie
        ff_ki          = p.ff_ki_water if is_water else p.ff_ki_auto
        integraal_zone = 2.0 if is_water else 1.0
        if ctrl.stand > 0 and abs(regel_fout) < integraal_zone:
            ctrl.ff_integraal += regel_fout * p.pid_interval_s
        ff_max_int = 3.0 * 3600.0 / ff_ki
        ctrl.ff_integraal = max(-ff_max_int, min(ff_max_int, ctrl.ff_integraal))
        int_corr = int(max(-2, min(2, ctrl.ff_integraal * ff_ki / 3600.0)))
        if regel_fout <= 0.0 and int_corr > 0:
            int_corr = 0

        max_stap = 3 if regel_fout > 3.0 else 1
        nieuwe_stand = max(0, min(8, stand_ff + int_corr))
        if t_outside < p.t_vorst:
            nieuwe_stand = max(1, nieuwe_stand)
        if ctrl.stand > 0:
            nieuwe_stand = max(max(0, ctrl.stand - max_stap),
                               min(min(8, ctrl.stand + max_stap), nieuwe_stand))

        ctrl.pid_output = stand_ff * 12.5

        # Online leren
        if p.learning_enabled:
            leer_drempel = 2.0 if is_water else 0.5
            p_hp_est = VERMOGEN[ctrl.stand] * self._ff_cop(t_supply, t_outside)
            if ctrl.stand > 0 and p_hp_est > 50.0 and abs(regel_fout) < leer_drempel:
                if is_water:
                    dt_sup = t_supply - t_kamer
                    if dt_sup > 2.0:
                        meting = p_hp_est / dt_sup
                        self.ff_ua_emitter = max(50.0, min(500.0,
                            (1 - p.ff_learn_rate) * self.ff_ua_emitter + p.ff_learn_rate * meting))
                else:
                    # Minimale ΔT=10°C: bij kleine ΔT (zachte buitentemperatuur) is de
                    # UA-meting 4× ruis-gevoeliger → UA loopt weg. Alleen leren bij
                    # voldoende ΔT zodat P/ΔT stabiel is.
                    dt_env = t_kamer - t_outside
                    if dt_env > 10.0:
                        meting = p_hp_est / dt_env
                        self.ff_ua_house = max(50.0, min(500.0,
                            (1 - p.ff_learn_rate) * self.ff_ua_house + p.ff_learn_rate * meting))

        # Hysteresis
        if nieuwe_stand < ctrl.stand:
            hyst = p.hyst_down_s
        elif regel_fout > coast_k:
            hyst = p.hyst_fast_s
        else:
            hyst = p.hyst_slow_s

        if nieuwe_stand != ctrl.stand and (now_s - ctrl.vorige_stand_wijz_s) >= hyst:
            ctrl.stand = nieuwe_stand
            ctrl.vorige_stand_wijz_s = now_s
            ctrl.wp_aan = ctrl.stand > 0
            if ctrl.stand == 0:
                ctrl.reset_ff()

    # ── Feedforward koelregelaar (ff_auto + ff_water in koeling modus) ─────────
    def _pas_ff_koel_aan(self, t_supply: float, t_kamer: float, t_outside: float,
                         now_s: float, is_water: bool):
        p    = self.p
        ctrl = self.ctrl

        # Minimale buitentemperatuur voor koeling (voorkomt koelen bij koud weer)
        if t_outside < p.koeling_min_buiten:
            if ctrl.wp_aan:
                ctrl.zet_uit()
            return

        # Koelsetpoint: ff_water gebruikt t_water_gewenst als koelwater-setpoint (bijv. 18°C)
        # ff_auto stuurt op kamertemperatuur
        wsp = p.t_water_gewenst if is_water else p.t_kamer_gewenst
        self.doel_setpoint = wsp

        # Regelafwijking koeling: positief = nog te warm = meer koeling nodig
        regel_fout = (t_supply - wsp) if is_water else (t_kamer - p.t_kamer_gewenst)

        # Condensatiebescherming: stop als aanvoer al op/onder supply_min
        if t_supply <= p.supply_min + 0.2:
            if ctrl.stand > 0 and (now_s - ctrl.vorige_stand_wijz_s) >= p.hyst_down_s:
                ctrl.stand = max(0, ctrl.stand - 1)
                ctrl.vorige_stand_wijz_s = now_s
                ctrl.wp_aan = ctrl.stand > 0
                if ctrl.stand == 0:
                    ctrl.wp_uit_s = now_s; ctrl.reset_ff()
            return

        # Te koud: terugschakelen (kamer/aanvoer heeft setpoint onderschreden)
        if regel_fout < -p.koeling_afschakel:
            if ctrl.stand > 0 and (now_s - ctrl.vorige_stand_wijz_s) >= p.hyst_down_s:
                ctrl.stand = max(0, ctrl.stand - 1)
                ctrl.vorige_stand_wijz_s = now_s
                ctrl.wp_aan = ctrl.stand > 0
                if ctrl.stand == 0:
                    ctrl.wp_uit_s = now_s; ctrl.reset_ff()
            return

        # Herstart vanuit stand 0: wacht tot voldoende warmtevraag
        if ctrl.stand == 0:
            min_off_ok = (now_s - ctrl.wp_uit_s) >= p.ff_thermal_min_off_s
            drempel_ok = regel_fout > p.ff_restart_coast
            if not (min_off_ok and drempel_ok):
                return

        # FF: benodigde koelvermogen
        if is_water:
            # Warmtestroom kamer → koud water op basis van UA_emitter
            p_nodig = max(0.0, self.ff_ua_emitter * (t_kamer - wsp))
            cop     = self._ff_cop_koel(t_supply, t_outside)
        else:
            # Warmte-inval vanuit buiten die weggekoeeld moet worden
            p_nodig = max(0.0, self.ff_ua_house * (t_outside - p.t_kamer_gewenst))
            cop     = self._ff_cop_koel(t_supply, t_outside)

        stand_ff = self._ff_stand_voor_vermogen(p_nodig / max(cop, 1.0))

        # Anticipatiezone: +1 als kamer nog ver boven setpoint
        coast_k = p.ff_coast_water if is_water else p.ff_coast_auto
        if regel_fout > coast_k:
            stand_ff = min(8, stand_ff + 1)

        # Integraalcorrectie (zelfde logica als verwarming)
        ff_ki          = p.ff_ki_water if is_water else p.ff_ki_auto
        integraal_zone = 2.0 if is_water else 1.0
        if ctrl.stand > 0 and abs(regel_fout) < integraal_zone:
            ctrl.ff_integraal += regel_fout * p.pid_interval_s
        ff_max_int = 3.0 * 3600.0 / ff_ki
        ctrl.ff_integraal = max(-ff_max_int, min(ff_max_int, ctrl.ff_integraal))
        int_corr = int(max(-2, min(2, ctrl.ff_integraal * ff_ki / 3600.0)))
        if regel_fout <= 0.0 and int_corr > 0:
            int_corr = 0

        max_stap = 3 if regel_fout > 3.0 else 1
        nieuwe_stand = max(0, min(8, stand_ff + int_corr))
        if ctrl.stand > 0:
            nieuwe_stand = max(max(0, ctrl.stand - max_stap),
                               min(min(8, ctrl.stand + max_stap), nieuwe_stand))

        ctrl.pid_output = stand_ff * 12.5

        # Hysteresis
        if nieuwe_stand < ctrl.stand:
            hyst = p.hyst_down_s
        elif regel_fout > coast_k:
            hyst = p.hyst_fast_s
        else:
            hyst = p.hyst_slow_s

        if nieuwe_stand != ctrl.stand and (now_s - ctrl.vorige_stand_wijz_s) >= hyst:
            ctrl.stand = nieuwe_stand
            ctrl.vorige_stand_wijz_s = now_s
            ctrl.wp_aan = ctrl.stand > 0
            if ctrl.stand == 0:
                ctrl.wp_uit_s = now_s; ctrl.reset_ff()

    # ── PID regelaar AUTO modus ───────────────────────────────────────────────
    def _pas_auto_aan(self, t_supply: float, t_kamer: float, t_outside: float, now_s: float):
        p    = self.p
        ctrl = self.ctrl
        dt_s = p.pid_interval_s

        stooklijn = self._stooklijn(t_outside)
        self.doel_setpoint = stooklijn

        if t_outside > p.stooklijn_uit:
            if ctrl.wp_aan:
                ctrl.zet_uit()
            return

        kamer_fout = p.t_kamer_gewenst - t_kamer

        # Noodstop kamer te warm
        absolute_max = min(p.t_kamer_gewenst + 0.5, 25.0)
        if t_kamer > absolute_max:
            ctrl.zet_uit()
            return

        # Vorstbeveiliging
        if t_outside < p.t_vorst and ctrl.stand == 0:
            ctrl.stand = 1; ctrl.wp_aan = True; ctrl.vorige_stand_wijz_s = now_s

        if kamer_fout > p.auto_aan_drempel:
            if not ctrl.wp_aan:
                ctrl.startup_s = now_s   # onthoud wanneer WP aansloeg
            ctrl.wp_aan = True
            aanvoer_fout = stooklijn - t_supply

            dt_correctie = 0.0
            delta_t = t_supply - t_kamer
            if delta_t < 4.0:
                dt_correctie = (delta_t - 5.0) * 3.0
            elif delta_t > 6.0:
                dt_correctie = (delta_t - 5.0) * 2.0

            # PID schaalfactor: firmware gebruikt 0.005 als vaste dt (= pid_interval_ms/1e6)
            # Ki/Kd zijn hierop gekalibreerd — gebruik dezelfde waarde
            PID_DT = 0.005

            diff = (aanvoer_fout - ctrl.pid_vorige_fout) / PID_DT
            ctrl.pid_vorige_fout = aanvoer_fout
            pid_raw = p.Kp * aanvoer_fout + p.Ki * ctrl.pid_integraal + p.Kd * diff + dt_correctie

            # Anti-windup
            if not ((pid_raw > 100.0 and aanvoer_fout > 0.0) or
                    (pid_raw <   0.0 and aanvoer_fout < 0.0)):
                ctrl.pid_integraal += aanvoer_fout * PID_DT
                ctrl.pid_integraal  = max(-50.0, min(50.0, ctrl.pid_integraal))

            ctrl.pid_output = max(0.0, min(100.0, pid_raw))

            # Stand lookup
            po = ctrl.pid_output
            if   po <  5: nieuwe_stand = 0
            elif po < 15: nieuwe_stand = 1
            elif po < 25: nieuwe_stand = 2
            elif po < 40: nieuwe_stand = 3
            elif po < 55: nieuwe_stand = 4
            elif po < 70: nieuwe_stand = 5
            elif po < 85: nieuwe_stand = 6
            elif po < 93: nieuwe_stand = 7
            else:         nieuwe_stand = 8

            # A: deadband — geen standwijziging als aanvoer dicht bij setpoint
            if abs(aanvoer_fout) < p.auto_aanvoer_deadband:
                nieuwe_stand = ctrl.stand   # huidige stand vasthouden

            # B: ±1 staplimiet per cycle — geen grote sprongen
            if nieuwe_stand > ctrl.stand + p.auto_max_stap:
                nieuwe_stand = ctrl.stand + p.auto_max_stap
            elif nieuwe_stand < ctrl.stand - p.auto_max_stap:
                nieuwe_stand = ctrl.stand - p.auto_max_stap

            # C: coldstart-ramp — lineair opbouwen over 30 min (was harde 15-min grens)
            elapsed = now_s - ctrl.startup_s
            if elapsed < 1800.0:
                max_stand_ramp = max(1, int(elapsed / 1800.0 * 8) + 1)
                nieuwe_stand = min(nieuwe_stand, max_stand_ramp)

            if t_outside < p.t_vorst and nieuwe_stand == 0:
                nieuwe_stand = 1

            hyst = p.hyst_fast_s if kamer_fout > 1.0 else p.hyst_slow_s
            if nieuwe_stand != ctrl.stand and (now_s - ctrl.vorige_stand_wijz_s) >= hyst:
                ctrl.stand = nieuwe_stand
                ctrl.vorige_stand_wijz_s = now_s

        elif kamer_fout < p.auto_uit_drempel:
            if t_outside < p.t_vorst:
                if ctrl.stand > 1:
                    ctrl.stand = 1; ctrl.wp_aan = True; ctrl.vorige_stand_wijz_s = now_s
            elif ctrl.stand > 0 and (now_s - ctrl.vorige_stand_wijz_s) >= p.auto_hyst_down_s:
                ctrl.stand -= 1
                ctrl.vorige_stand_wijz_s = now_s
                ctrl.wp_aan = ctrl.stand > 0
                if ctrl.stand == 0:
                    ctrl.reset_pid()

    # ── PID regelaar WATER modus ─────────────────────────────────────────────
    def _pas_water_aan(self, t_supply: float, t_kamer: float, t_outside: float, now_s: float):
        p    = self.p
        ctrl = self.ctrl

        # In het operationele systeem stuurt Adam de stooklijn als water setpoint via MQTT
        wsp = self._stooklijn(t_outside)
        water_fout = wsp - t_supply
        self.doel_setpoint = wsp

        if t_outside < p.t_vorst and ctrl.stand == 0:
            ctrl.stand = 1; ctrl.wp_aan = True; ctrl.vorige_stand_wijz_s = now_s

        if water_fout > 1.0:
            ctrl.wp_aan = True
        elif water_fout < -1.0:
            if t_outside >= p.t_vorst:
                ctrl.zet_uit()
            return

        if ctrl.wp_aan:
            PID_DT = 0.005
            diff = (water_fout - ctrl.pid_vorige_fout) / PID_DT
            ctrl.pid_vorige_fout = water_fout
            ctrl.pid_integraal = max(-50.0, min(50.0, ctrl.pid_integraal + water_fout * PID_DT))
            ctrl.pid_output = max(-100.0, min(100.0,
                p.Kp_water * water_fout + p.Ki_water * ctrl.pid_integraal + p.Kd_water * diff))

            po = ctrl.pid_output
            if   po <  5: nieuwe_stand = 0
            elif po < 15: nieuwe_stand = 1
            elif po < 25: nieuwe_stand = 2
            elif po < 40: nieuwe_stand = 3
            elif po < 55: nieuwe_stand = 4
            elif po < 70: nieuwe_stand = 5
            elif po < 85: nieuwe_stand = 6
            elif po < 93: nieuwe_stand = 7
            else:         nieuwe_stand = 8

            if t_outside < p.t_vorst and nieuwe_stand == 0:
                nieuwe_stand = 1

            hyst = (p.hyst_fast_s if water_fout > 5.0 else p.hyst_slow_s) \
                   if nieuwe_stand > ctrl.stand else p.hyst_down_s

            if nieuwe_stand != ctrl.stand and (now_s - ctrl.vorige_stand_wijz_s) >= hyst:
                ctrl.stand = nieuwe_stand
                ctrl.vorige_stand_wijz_s = now_s
                ctrl.wp_aan = ctrl.stand > 0
                if ctrl.stand == 0:
                    ctrl.reset_pid()

    # ── Hoofd-update (wordt elke pid_interval aangeroepen) ───────────────────
    def update(self, t_supply: float, t_kamer: float, t_outside: float,
               now_s: float, modus: str):
        self._modus = modus
        if   modus in ("ff_auto", "ff_water"):
            self._pas_ff_aan(t_supply, t_kamer, t_outside, now_s)
        elif modus == "auto":
            self._pas_auto_aan(t_supply, t_kamer, t_outside, now_s)
        elif modus == "water":
            self._pas_water_aan(t_supply, t_kamer, t_outside, now_s)
        # handmatig: stand van buiten gezet, niets doen

    @property
    def stand(self): return self.ctrl.stand


# ═══════════════════════════════════════════════════════════════════════════════
#  SCENARIO DEFINITIE
# ═══════════════════════════════════════════════════════════════════════════════
@dataclass
class Scenario:
    modus:        str   = "ff_auto"
    days:         float = 5.0           # simulatieduur in dagen
    t_outside:    float = 5.0           # basistemperatuur buiten
    daily_swing:  float = 5.0           # amplitude dagvariatie (°C)
    t_kamer_init: float = 20.0
    t_supply_init: float = 32.0
    # Optioneel: setpoint-schema [(sim_hour, setpoint), ...]
    setpoint_schedule: list = field(default_factory=list)


def outside_temp(t_sim_s: float, scenario: Scenario) -> float:
    if scenario.daily_swing == 0:
        return scenario.t_outside
    hour = (t_sim_s / 3600.0) % 24
    return scenario.t_outside + scenario.daily_swing * math.sin(math.pi * (hour - 6) / 12)


def get_setpoint(t_sim_s: float, scenario: Scenario, default: float) -> float:
    if not scenario.setpoint_schedule:
        return default
    h = t_sim_s / 3600.0
    sp = default
    for (h_start, new_sp) in scenario.setpoint_schedule:
        if h >= h_start:
            sp = new_sp
    return sp


# ═══════════════════════════════════════════════════════════════════════════════
#  SIMULATIEKERN
# ═══════════════════════════════════════════════════════════════════════════════
def run_scenario(scenario: Scenario, params: Params,
                 dt_house_s: float = 1.0,
                 csv_path: Optional[str] = None,
                 verbose: bool = True) -> dict:
    """
    Voert de simulatie uit en geeft statistieken terug als dict.
    dt_house_s: tijdstap huismodel (seconden) — kleiner = nauwkeuriger, langzamer
    """
    house  = HouseModel(t_kamer=scenario.t_kamer_init, t_supply=scenario.t_supply_init)
    ctrl   = Controller(params)
    modus  = scenario.modus

    total_s   = scenario.days * 86400
    pid_dt_s  = params.pid_interval_s

    # Statistieken
    samples_kamer    = []
    samples_aanvoer  = []
    stand_histogram  = [0] * 9
    switches         = 0
    prev_stand       = 0

    # CSV
    csv_file = csv_writer = None
    if csv_path:
        csv_file   = open(csv_path, "w", newline="")
        csv_writer = csv.writer(csv_file)
        csv_writer.writerow(["sim_h", "t_kamer", "t_supply", "t_outside",
                              "stand", "pid_out", "ua", "setpoint", "doel_sp"])

    t_sim_s    = 0.0
    next_pid_s = 0.0

    # Header
    if verbose:
        is_water_m = modus in ("water", "ff_water")
        ref_lbl  = "aanvoer_fout" if is_water_m else "kamer_fout"
        print(f"\nSimulatie: modus={modus}  buiten={scenario.t_outside:.1f}°C ±{scenario.daily_swing:.1f}°C  "
              f"duur={scenario.days:.1f}d")
        print(f"{'─'*85}")
        print(f"{'tijd':>8}  {'stand':>5}  {'t_kamer':>8}  {'t_supply':>9}  "
              f"{'t_buiten':>8}  {ref_lbl:>11}  {'UA':>7}  {'pid%':>5}")
        print(f"{'─'*85}")

    while t_sim_s < total_s:
        t_out = outside_temp(t_sim_s, scenario)

        # Update setpoint vanuit schema
        params.t_kamer_gewenst = get_setpoint(t_sim_s, scenario, params.t_kamer_gewenst)

        # Controller update (elke pid_interval_s)
        if t_sim_s >= next_pid_s:
            ctrl.update(house.t_supply, house.t_kamer, t_out, t_sim_s, modus)
            next_pid_s += pid_dt_s

            # Stand wisseling tellen
            if ctrl.stand != prev_stand:
                switches += 1
                prev_stand = ctrl.stand

            # Console output (elke 30 min sim)
            if verbose and int(t_sim_s) % 1800 < pid_dt_s:
                h, m = divmod(int(t_sim_s) // 60, 60)
                is_wm = modus in ("water", "ff_water")
                fout  = (ctrl.doel_setpoint - house.t_supply) if is_wm \
                        else (params.t_kamer_gewenst - house.t_kamer)
                ua    = ctrl.ff_ua_emitter if modus == "ff_water" else ctrl.ff_ua_house
                print(f"{h:4d}h{m:02d}  {ctrl.stand:5d}  "
                      f"{house.t_kamer:8.2f}°C  {house.t_supply:9.2f}°C  "
                      f"{t_out:8.1f}°C  {fout:+11.2f}°C  "
                      f"{ua:7.1f}  {ctrl.ctrl.pid_output:5.1f}%")

        # Huis integreer-stap
        t_sup, t_ret, t_kam, cop, p_heat = house.step(
            ctrl.stand, t_out, dt_house_s,
            koeling=params.koeling_modus, supply_min=params.supply_min)

        # Veiligheidsgrens (firmware: SUPPLY_MAX)
        if t_sup > params.supply_max:
            ctrl.ctrl.zet_uit()

        # Statistieken accumuleren (elke pid_interval)
        if t_sim_s >= next_pid_s - pid_dt_s:
            is_wm = modus in ("water", "ff_water")
            ref   = ctrl.doel_setpoint if is_wm else params.t_kamer_gewenst
            val   = t_sup if is_wm else t_kam
            samples_kamer.append(val - ref)
            samples_aanvoer.append(t_sup)
            stand_histogram[min(ctrl.stand, 8)] += 1

        # CSV
        if csv_writer and int(t_sim_s) % 60 < dt_house_s:
            ua = ctrl.ff_ua_emitter if modus == "ff_water" else ctrl.ff_ua_house
            csv_writer.writerow([
                f"{t_sim_s/3600:.3f}",
                f"{t_kam:.3f}", f"{t_sup:.3f}", f"{t_out:.2f}",
                ctrl.stand, f"{ctrl.ctrl.pid_output:.1f}",
                f"{ua:.1f}", f"{params.t_kamer_gewenst:.1f}",
                f"{ctrl.doel_setpoint:.1f}"
            ])

        t_sim_s += dt_house_s

    if csv_file:
        csv_file.close()

    # Statistieken
    n = len(samples_kamer) or 1
    errors = samples_kamer
    rmse   = math.sqrt(sum(e*e for e in errors) / n)
    bias   = sum(errors) / n
    pct_05 = sum(1 for e in errors if abs(e) < 0.5) / n * 100
    pct_10 = sum(1 for e in errors if abs(e) < 1.0) / n * 100

    result = dict(
        rmse=rmse, bias=bias, pct_05=pct_05, pct_10=pct_10,
        switches=switches,
        ua_house_end=ctrl.ff_ua_house, ua_emitter_end=ctrl.ff_ua_emitter,
        stand_hist=stand_histogram,
    )

    if verbose:
        is_wm = modus in ("water", "ff_water")
        lbl   = "aanvoer" if is_wm else "kamer"
        print(f"\n{'═'*60}")
        print(f"  RMSE {lbl}: {rmse:.3f}°C   bias: {bias:+.3f}°C")
        print(f"  Binnen ±0.5°C: {pct_05:.1f}%   Binnen ±1.0°C: {pct_10:.1f}%")
        print(f"  Stand-wisselingen: {switches}")
        if ctrl.ff_ua_house != params.ff_ua_house:
            print(f"  UA_house:   {params.ff_ua_house:.1f} → {ctrl.ff_ua_house:.1f} W/K")
        if ctrl.ff_ua_emitter != params.ff_ua_emitter:
            print(f"  UA_emitter: {params.ff_ua_emitter:.1f} → {ctrl.ff_ua_emitter:.1f} W/K")
        print(f"  Stand verdeling: " + "  ".join(
            f"st{i}:{v/n*100:.0f}%" for i, v in enumerate(stand_histogram) if v > 0))
        if csv_path:
            print(f"  CSV opgeslagen: {csv_path}")

    return result


# ═══════════════════════════════════════════════════════════════════════════════
#  PARAMETER OPTIMALISATIE (grid search)
# ═══════════════════════════════════════════════════════════════════════════════
def optimise(scenario: Scenario, base_params: Params):
    """Eenvoudige grid search op één of meer parameters."""
    import itertools

    # Definieer het zoekgrid hier:
    if scenario.modus == "auto":
        grid = {
            "Kp": [15.0, 19.9, 25.0],
            "Ki": [0.050, 0.084, 0.120],
            "Kd": [0.020, 0.036, 0.060],
        }
    else:
        grid = {
            "ff_ua_house":   [250.0, 272.5, 295.0],
            "ff_ua_emitter": [230.0, 250.0, 270.0],
            "ff_coast_auto": [0.20, 0.30, 0.45],
        }

    keys   = list(grid.keys())
    combos = list(itertools.product(*[grid[k] for k in keys]))
    print(f"Optimalisatie: {len(combos)} combinaties voor modus={scenario.modus}")
    print(f"{'─'*70}")

    best_rmse = float("inf")
    best_combo = None

    for combo in combos:
        import copy
        p = copy.deepcopy(base_params)
        for k, v in zip(keys, combo):
            setattr(p, k, v)
        result = run_scenario(scenario, p, dt_house_s=5.0, verbose=False)
        label  = "  ".join(f"{k}={v}" for k, v in zip(keys, combo))
        print(f"  {label}  →  RMSE={result['rmse']:.3f}°C  ±0.5={result['pct_05']:.0f}%")
        if result["rmse"] < best_rmse:
            best_rmse  = result["rmse"]
            best_combo = dict(zip(keys, combo))

    print(f"\n{'═'*60}")
    print(f"  Beste resultaat: RMSE={best_rmse:.3f}°C")
    for k, v in best_combo.items():
        print(f"    {k} = {v}")


# ═══════════════════════════════════════════════════════════════════════════════
#  CLI
# ═══════════════════════════════════════════════════════════════════════════════
def parse_args():
    p = argparse.ArgumentParser(
        description="Pure Python WP controller + huis simulatie (geen MQTT/Arduino nodig)")
    p.add_argument("--modus",    default="ff_auto",
                   choices=["auto","water","ff_auto","ff_water","handmatig"],
                   help="Regelaarsmodus (default: ff_auto)")
    p.add_argument("--days",     default=5.0,  type=float, help="Simulatieduur (dagen)")
    p.add_argument("--outside",  default=5.0,  type=float, help="Basistemperatuur buiten (°C)")
    p.add_argument("--swing",    default=5.0,  type=float, help="Dagelijkse temp-amplitude (°C)")
    p.add_argument("--setpoint", default=20.0, type=float, help="Kamer setpoint (°C)")
    p.add_argument("--water-sp", default=32.0, type=float, help="Water setpoint °C (water-modi)")
    p.add_argument("--dt",       default=1.0,  type=float, help="Huismodel tijdstap (s, default 1)")
    p.add_argument("--csv",      default="",             help="Sla resultaten op als CSV")
    p.add_argument("--quiet",    action="store_true",    help="Geen console output tijdens simulatie")
    p.add_argument("--optimise", action="store_true",    help="Parameter grid-search")
    p.add_argument("--koeling",  action="store_true",
                   help="Koelmodus (FF_AUTO/FF_WATER/handmatig); buiten-default 28°C, setpoint-default 22°C, water-sp-default 18°C)")

    # Parameter overrides
    p.add_argument("--kp",   type=float, help="Kp overschrijven")
    p.add_argument("--ki",   type=float, help="Ki overschrijven")
    p.add_argument("--kd",   type=float, help="Kd overschrijven")
    p.add_argument("--ua-house",   type=float, help="ff_ua_house overschrijven (W/K)")
    p.add_argument("--ua-emitter", type=float, help="ff_ua_emitter overschrijven (W/K)")
    p.add_argument("--no-learning", action="store_true", help="UA leren uitschakelen")
    p.add_argument("--supply-min", default=17.0, type=float,
                   help="Condensatiebescherming: min aanvoertemp koeling (default 17°C)")
    return p.parse_args()


if __name__ == "__main__":
    args = parse_args()

    params = Params()
    params.t_kamer_gewenst = args.setpoint
    params.t_water_gewenst = args.water_sp
    if args.kp:           params.Kp            = args.kp
    if args.ki:           params.Ki            = args.ki
    if args.kd:           params.Kd            = args.kd
    if args.ua_house:     params.ff_ua_house   = args.ua_house
    if args.ua_emitter:   params.ff_ua_emitter = args.ua_emitter
    if args.no_learning:  params.learning_enabled = False
    if args.koeling:
        params.koeling_modus = True
        params.supply_min    = args.supply_min
        # Verstandige defaults voor koeling als de gebruiker ze niet expliciet opgeeft
        if args.outside == 5.0:   args.outside  = 28.0   # zomerdag
        if args.swing   == 5.0:   args.swing    = 6.0    # dag/nacht verschil
        if args.setpoint == 20.0: args.setpoint = 22.0   # koel setpoint
        if args.water_sp == 32.0: args.water_sp = 18.0   # koel watersetpoint
        params.t_kamer_gewenst = args.setpoint
        params.t_water_gewenst = args.water_sp

    scenario = Scenario(
        modus       = args.modus,
        days        = args.days,
        t_outside   = args.outside,
        daily_swing = args.swing,
        t_kamer_init  = args.setpoint + 2.0 if args.koeling else args.setpoint,
        t_supply_init = 20.0 if args.koeling else 32.0,
    )

    if args.optimise:
        optimise(scenario, params)
    else:
        run_scenario(scenario, params,
                     dt_house_s=args.dt,
                     csv_path=args.csv or None,
                     verbose=not args.quiet)
