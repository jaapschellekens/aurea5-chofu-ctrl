#!/usr/bin/env python3
"""
stookseizoen_sim.py — Stookseizoen simulatie NL (oktober–april)

Simuleert alle vier regelaarsmodi over een volledig Nederlands stookseizoen
(212 dagen, 1 okt – 30 apr) met een realistisch klimaatprofiel gebaseerd op
KNMI klimatologie.

Gebruik:
    python stookseizoen_sim.py                        # alle modi vergelijken
    python stookseizoen_sim.py --modus ff_auto        # één modus
    python stookseizoen_sim.py --csv resultaten.csv   # bewaar alle data
    python stookseizoen_sim.py --optimise --modus auto   # PID optimalisatie

KGE-definitie (Kling-Gupta Efficiency):
    KGE = 1 - √((r-1)² + (α-1)² + (β-1)²)
    sim = aanvoer temperatuur  |  obs = doel_setpoint (stooklijn of water setpoint)

    Voor modi met een varierende referentie (stooklijn): standaard KGE.
    Voor plain 'water' mode (constante referentie 32°C): gemodificeerde KGE
        KGE_const = 1 - √((std_sim/σ_ref)² + (β-1)²)
    waarbij σ_ref=2°C. Perfect bij std_sim=0 en mean_sim=setpoint → KGE=1.

Seizoenstemperatuur NL (bron: KNMI klimaatnormen 1991-2020):
    Okt: 11°C  Nov: 7°C  Dec: 4°C  Jan: 3°C  Feb: 4°C  Mrt: 7°C  Apr: 11°C
    Dagelijkse variatie: ±4°C (min om 06:00, max om 14:00)
"""
import argparse
import csv
import math
import sys
import os
import copy
from dataclasses import dataclass, field
from typing import Optional, List

# Voeg python-map toe aan sys.path zodat we wp_controller_sim kunnen importeren
sys.path.insert(0, os.path.dirname(__file__))
from wp_controller_sim import (
    Params, ControllerState, HouseModel, Controller, VERMOGEN,
    UA_HOUSE_SIM, C_TH, UA_EMITTER_SIM, C_WATER, COP_ETA, G_FLOW,
)

# ─── Maandnamen voor rapportage ──────────────────────────────────────────────
MAANDEN = ["okt", "nov", "dec", "jan", "feb", "mrt", "apr"]

# Startdag in het jaar (0=1jan): 1 okt = dag 273
SEIZOEN_START_DAG = 273
SEIZOEN_DAGEN     = 212    # 1 okt t/m 30 apr


# ═══════════════════════════════════════════════════════════════════════════════
#  KLIMAATPROFIEL NEDERLAND (KNMI 1991-2020)
# ═══════════════════════════════════════════════════════════════════════════════
def nl_buiten_temp(t_sim_s: float, jaar_variatie: float = 1.0) -> float:
    """
    Geeft buitentemperatuur terug voor tijdstip t_sim_s seconden na 1 okt.

    Seizoenscomponent: sinusoïde gefittd op KNMI maandgemiddelden
        min ≈ 3°C op 15 jan (dag 15 v/h jaar)
        max ≈ 18°C op 15 jul (dag 196 v/h jaar)
    Dagcomponent: ±4°C, minimum om 06:00, maximum om 14:00.
    jaar_variatie: schaalfactor voor seizoensamplitude (1.0 = normaal jaar)
    """
    dag_in_jaar = (SEIZOEN_START_DAG + t_sim_s / 86400.0) % 365.0
    uur = (t_sim_s / 3600.0) % 24.0

    # Seizoenstemperatuur: T = 10.5 - 7.5 × cos(2π × dag/365)
    # Dag 0 = 1 jan → minimum op dag 15
    t_seizoen = 10.5 - 7.5 * jaar_variatie * math.cos(2 * math.pi * (dag_in_jaar - 15) / 365)

    # Dagelijkse variatie: ±4°C
    t_dag = 4.0 * math.sin(math.pi * (uur - 6.0) / 12.0)

    return t_seizoen + t_dag


def maand_voor_dag(dag_van_seizoen: float) -> str:
    """Geeft maandnaam voor dag t.o.v. seizoensstart (dag 0 = 1 okt)."""
    # Grenzen in dagen van seizoen:
    grenzen = [0, 31, 61, 92, 123, 151, 182, 212]  # okt,nov,dec,jan,feb,mrt,apr
    for i, g in enumerate(grenzen[1:], 1):
        if dag_van_seizoen < g:
            return MAANDEN[i - 1]
    return MAANDEN[-1]


# ═══════════════════════════════════════════════════════════════════════════════
#  KGE (Kling-Gupta Efficiency) — online berekening
# ═══════════════════════════════════════════════════════════════════════════════
def kge_uit_accumulatie(n, sum_s, sum_o, sum_s2, sum_o2, sum_so,
                        constante_ref: bool = False, sigma_ref: float = 2.0):
    """
    Berekent KGE uit online-geaccumuleerde statistieken.

    sim = aanvoer temperatuur
    obs = doel_setpoint (stooklijn of water setpoint)

    Standaard KGE (voor varierende referentie, stooklijn-modi):
        KGE = 1 - √((r-1)² + (α-1)² + (β-1)²)
        r = Pearson correlatie(sim, obs)
        α = std(sim) / std(obs)          # variabiliteitsratio
        β = mean(sim) / mean(obs)        # biasverhouding

    Gemodificeerde KGE (voor constante referentie, water mode):
        KGE_const = 1 - √((std_sim/σ_ref)² + (β-1)²)
        Perfect bij std_sim=0 en mean_sim=obs → KGE=1
        σ_ref: aanvaardbare spreiding in aanvoertemperatuur (default 2°C)
    """
    if n < 2:
        return -9999.0, 0.0, 0.0, 0.0

    mean_s = sum_s / n
    mean_o = sum_o / n
    var_s  = max(0.0, sum_s2 / n - mean_s ** 2)
    var_o  = max(0.0, sum_o2 / n - mean_o ** 2)
    std_s  = math.sqrt(var_s)
    std_o  = math.sqrt(var_o)

    beta = mean_s / mean_o if abs(mean_o) > 0.01 else 1.0

    if constante_ref:
        # Gemodificeerde KGE voor constante referentie.
        # std én bias genormaliseerd met σ_ref → KGE_const = 1 - RMSE/σ_ref
        # (equivalente met α=std/σ_ref en β_norm=bias/σ_ref)
        # Dit geeft een eerlijke straf op zowel variatie als systematische afwijking.
        bias_abs = mean_s - mean_o
        alpha    = std_s    / sigma_ref
        beta_n   = bias_abs / sigma_ref     # genormaliseerde bias (niet ratio)
        kge      = 1.0 - math.sqrt(alpha ** 2 + beta_n ** 2)
        return kge, 1.0, alpha, beta        # β als ratio voor leesbare output
    else:
        # Standaard KGE
        cov = sum_so / n - mean_s * mean_o
        denom = std_s * std_o
        r = max(-1.0, min(1.0, cov / denom)) if denom > 1e-9 else 1.0
        alpha = std_s / std_o if std_o > 1e-9 else (0.0 if std_s < 1e-9 else 9999.0)
        kge   = 1.0 - math.sqrt((r - 1) ** 2 + (alpha - 1) ** 2 + (beta - 1) ** 2)
        return kge, r, alpha, beta


# ═══════════════════════════════════════════════════════════════════════════════
#  SEIZOEN SIMULATIEKERN
# ═══════════════════════════════════════════════════════════════════════════════
@dataclass
class MaandStat:
    naam:         str
    n_samples:    int   = 0
    som_kamer:    float = 0.0
    som_kamer2:   float = 0.0
    som_aanvoer:  float = 0.0
    som_fout:     float = 0.0
    som_fout2:    float = 0.0
    som_p_elec:   float = 0.0   # W (voor energieberekening)
    som_p_heat:   float = 0.0   # W
    stand_hist:   List[int] = field(default_factory=lambda: [0] * 9)
    wisselingen:  int   = 0

    def voeg_toe(self, t_kamer, t_supply, fout, p_elec, p_heat, stand):
        self.n_samples   += 1
        self.som_kamer   += t_kamer
        self.som_kamer2  += t_kamer ** 2
        self.som_aanvoer += t_supply
        self.som_fout    += fout
        self.som_fout2   += fout ** 2
        self.som_p_elec  += p_elec
        self.som_p_heat  += p_heat
        if 0 <= stand <= 8:
            self.stand_hist[stand] += 1

    @property
    def gem_kamer(self): return self.som_kamer / max(1, self.n_samples)
    @property
    def gem_aanvoer(self): return self.som_aanvoer / max(1, self.n_samples)
    @property
    def bias(self):     return self.som_fout  / max(1, self.n_samples)
    @property
    def rmse(self):     return math.sqrt(self.som_fout2 / max(1, self.n_samples))
    @property
    def elec_kwh(self): return self.som_p_elec * 5.0 / 3_600_000  # pid_dt=5s
    @property
    def heat_kwh(self): return self.som_p_heat * 5.0 / 3_600_000
    @property
    def gem_cop(self):  return self.heat_kwh / max(0.001, self.elec_kwh)
    @property
    def pct_05(self):
        # berekend op basis van fout-drempel; we bewaren niet individueel, dus schat via rmse
        # (exacte berekening zou opslag van alle samples vereisen)
        return None


def run_seizoen(
    modus:       str,
    params:      Params,
    dt_house_s:  float = 5.0,       # tijdstap huismodel (s)
    csv_path:    Optional[str] = None,
    verbose:     bool  = True,
    jaar_var:    float = 1.0,        # seizoensamplitude schaalfactor
) -> dict:
    """
    Simuleert één stookseizoen (212 dagen) voor de gegeven modus.
    Geeft dict terug met maandelijkse en totale statistieken.
    """
    house  = HouseModel(t_kamer=20.0, t_supply=28.0)
    ctrl   = Controller(params)

    total_s   = SEIZOEN_DAGEN * 86400
    pid_dt_s  = params.pid_interval_s

    # Maandstatistieken
    maand_grenzen = [0, 31, 61, 92, 123, 151, 182, 212]
    maand_stats   = [MaandStat(MAANDEN[i]) for i in range(len(MAANDEN))]

    # Totaalstatistieken
    # Statistieken
    all_fouten  : list = []
    wisselingen  = 0
    starts       = 0      # inschakelingen: stand 0 → >0
    prev_stand   = 0
    next_pid_s   = 0.0
    t_sim_s      = 0.0

    # KGE accumulatie: sim=val, obs=ref (zelfde als RMSE)
    kge_n = kge_ss = kge_so = kge_ss2 = kge_so2 = kge_sso = 0.0
    # water/ff_water: variabele referentie (stooklijn via Adam) → standaard KGE met correlatie
    # auto / ff_auto: constante kamertemperatuur referentie → KGE_const
    _constante_ref = modus not in ("water", "ff_water")
    _sigma_ref = 2.0 if modus in ("water", "ff_water") else 1.0  # °C (ongebruikt bij standaard KGE)

    # CSV
    csv_file = csv_writer = None
    if csv_path:
        csv_file   = open(csv_path, "w", newline="")
        csv_writer = csv.writer(csv_file)
        csv_writer.writerow([
            "dag", "uur", "maand", "t_kamer", "t_supply", "t_buiten",
            "stand", "doel_sp", "fout", "p_elec_W", "p_heat_W",
            "cop", "pid_out", "ua_house", "ua_emitter",
        ])

    if verbose:
        is_wm = modus in ("water", "ff_water")
        print(f"\n{'═'*90}")
        print(f"  Modus: {modus.upper():10s}  |  Stookseizoen NL (1 okt – 30 apr, {SEIZOEN_DAGEN} dagen)")
        print(f"{'═'*90}")
        print(f"{'datum':>8}  {'stand':>5}  {'t_kamer':>8}  {'t_supply':>9}  "
              f"{'t_buiten':>8}  {'fout':>7}  {'UA_h':>6}  "
              f"{'kWh_el':>7}  {'COP':>4}")
        print(f"{'─'*90}")

    while t_sim_s < total_s:
        dag     = t_sim_s / 86400.0
        t_out   = nl_buiten_temp(t_sim_s, jaar_var)

        if t_sim_s >= next_pid_s:
            ctrl.update(house.t_supply, house.t_kamer, t_out, t_sim_s, modus)
            next_pid_s += pid_dt_s

            if ctrl.stand != prev_stand:
                wisselingen += 1
                if prev_stand == 0 and ctrl.stand > 0:
                    starts += 1
                prev_stand = ctrl.stand

        # Huis stap
        t_sup, t_ret, t_kam, cop, p_heat = house.step(ctrl.stand, t_out, dt_house_s)
        p_elec = float(VERMOGEN[max(0, min(ctrl.stand, 12))])

        if t_sup > params.supply_max:
            ctrl.ctrl.zet_uit()

        # Statistieken (elke pid-interval)
        if t_sim_s >= next_pid_s - pid_dt_s:
            is_wm = modus in ("water", "ff_water")
            ref   = ctrl.doel_setpoint if is_wm else params.t_kamer_gewenst
            val   = t_sup              if is_wm else t_kam
            fout  = val - ref

            all_fouten.append(fout)

            # KGE accumulatie — zelfde sim/obs als RMSE (is_wm bepaald hierboven)
            s = val   # t_supply (water-modi) of t_kamer (auto-modi)
            o = ref   # doel_setpoint (water-modi) of t_kamer_gewenst (auto-modi)
            kge_n   += 1
            kge_ss  += s;   kge_so  += o
            kge_ss2 += s*s; kge_so2 += o*o; kge_sso += s*o

            # Maand toewijzen
            mi = 0
            for i, g in enumerate(maand_grenzen[1:], 1):
                if dag < g:
                    mi = i - 1
                    break
            else:
                mi = len(MAANDEN) - 1
            maand_stats[mi].voeg_toe(t_kam, t_sup, fout, p_elec, p_heat, ctrl.stand)

        # CSV (elke 15 minuten gesimuleerd)
        if csv_writer and int(t_sim_s) % 900 < dt_house_s:
            dag_v_mnd = dag
            is_wm = modus in ("water", "ff_water")
            ref   = ctrl.doel_setpoint if is_wm else params.t_kamer_gewenst
            csv_writer.writerow([
                f"{dag:.4f}",
                f"{(t_sim_s / 3600.0) % 24:.2f}",
                maand_voor_dag(dag),
                f"{t_kam:.3f}", f"{t_sup:.3f}", f"{t_out:.2f}",
                ctrl.stand,
                f"{ctrl.doel_setpoint:.2f}",
                f"{t_kam - ref:.3f}",
                f"{p_elec:.0f}", f"{p_heat:.0f}",
                f"{cop:.2f}",
                f"{ctrl.ctrl.pid_output:.1f}",
                f"{ctrl.ff_ua_house:.1f}",
                f"{ctrl.ff_ua_emitter:.1f}",
            ])

        # Console (elke ~7 dagen = 1 week simtijd)
        if verbose and int(t_sim_s) % (7 * 86400) < dt_house_s:
            dag_int  = int(dag)
            maand    = maand_voor_dag(dag_int)
            is_wm    = modus in ("water", "ff_water")
            ref      = ctrl.doel_setpoint if is_wm else params.t_kamer_gewenst
            fout_now = (t_sup if is_wm else t_kam) - ref
            # Energie van de huidige maandstat
            mi_nu = 0
            for i, g in enumerate(maand_grenzen[1:], 1):
                if dag < g:
                    mi_nu = i - 1; break
            ms_nu = maand_stats[mi_nu]
            print(f"{maand:>3} d{dag_int:03d}  {ctrl.stand:5d}  "
                  f"{t_kam:8.2f}°C  {t_sup:9.2f}°C  "
                  f"{t_out:8.1f}°C  {fout_now:+7.2f}°C  "
                  f"{ctrl.ff_ua_house:6.1f}  "
                  f"{ms_nu.elec_kwh:7.1f}  "
                  f"{ms_nu.gem_cop:4.2f}")

        t_sim_s += dt_house_s

    if csv_file:
        csv_file.close()

    # Totaalstatistieken
    n       = len(all_fouten) or 1
    rmse    = math.sqrt(sum(e*e for e in all_fouten) / n)
    bias    = sum(all_fouten) / n
    pct_05  = sum(1 for e in all_fouten if abs(e) < 0.5) / n * 100
    pct_10  = sum(1 for e in all_fouten if abs(e) < 1.0) / n * 100
    tot_kwh = sum(ms.elec_kwh for ms in maand_stats)
    tot_kwh_heat = sum(ms.heat_kwh for ms in maand_stats)
    gem_cop = tot_kwh_heat / max(0.001, tot_kwh)

    kge, kge_r, kge_alpha, kge_beta = kge_uit_accumulatie(
        int(kge_n), kge_ss, kge_so, kge_ss2, kge_so2, kge_sso,
        constante_ref=_constante_ref, sigma_ref=_sigma_ref,
    )

    if verbose:
        print(f"\n{'─'*90}")
        print(f"  {'Maand':6}  {'t_kamer':>8}  {'t_aanvoer':>9}  "
              f"{'bias':>7}  {'RMSE':>6}  {'kWh_el':>8}  {'kWh_th':>8}  "
              f"{'COP':>5}  {'%stand0':>8}")
        print(f"  {'─'*84}")
        for ms in maand_stats:
            if ms.n_samples == 0:
                continue
            n_ms = ms.n_samples
            pct0 = ms.stand_hist[0] / n_ms * 100
            print(f"  {ms.naam:6}  {ms.gem_kamer:8.2f}°C  {ms.gem_aanvoer:9.2f}°C  "
                  f"{ms.bias:+7.3f}°C  {ms.rmse:6.3f}°C  "
                  f"{ms.elec_kwh:8.1f}  {ms.heat_kwh:8.1f}  "
                  f"{ms.gem_cop:5.2f}  {pct0:7.1f}%")
        print(f"  {'─'*84}")
        print(f"  {'TOTAAL':6}                             "
              f"         {bias:+7.3f}°C  {rmse:6.3f}°C  "
              f"{tot_kwh:8.1f}  {tot_kwh_heat:8.1f}  {gem_cop:5.2f}")
        print(f"\n  Binnen ±0.5°C: {pct_05:.1f}%   Binnen ±1.0°C: {pct_10:.1f}%   "
              f"Stand-wisselingen: {wisselingen}   Inschakelingen: {starts}")
        kge_label = "KGE_const" if _constante_ref else "KGE      "
        ref_desc  = ("t_supply vs stooklijn"    if modus in ("water", "ff_water")
                     else "t_kamer vs setpoint")
        print(f"  {kge_label}: {kge:.4f}   r={kge_r:.3f}   α={kge_alpha:.3f}   β={kge_beta:.4f}")
        ref_extra = f", σ_ref={_sigma_ref:.1f}°C" if _constante_ref else ""
        print(f"  (sim={ref_desc}{ref_extra})")
        if ctrl.ff_ua_house != params.ff_ua_house:
            print(f"  UA_house:   {params.ff_ua_house:.1f} → {ctrl.ff_ua_house:.1f} W/K")
        if ctrl.ff_ua_emitter != params.ff_ua_emitter:
            print(f"  UA_emitter: {params.ff_ua_emitter:.1f} → {ctrl.ff_ua_emitter:.1f} W/K")
        if csv_path:
            print(f"  CSV: {csv_path}")

    return dict(
        modus=modus, rmse=rmse, bias=bias, pct_05=pct_05, pct_10=pct_10,
        wisselingen=wisselingen, starts=starts,
        elec_kwh=tot_kwh, heat_kwh=tot_kwh_heat, gem_cop=gem_cop,
        kge=kge, kge_r=kge_r, kge_alpha=kge_alpha, kge_beta=kge_beta,
        ua_house_end=ctrl.ff_ua_house, ua_emitter_end=ctrl.ff_ua_emitter,
        maand_stats=maand_stats,
    )


# ═══════════════════════════════════════════════════════════════════════════════
#  PID OPTIMALISATIE — grid search over het volledige stookseizoen
# ═══════════════════════════════════════════════════════════════════════════════
def _run_combo(args_tuple):
    """Worker voor multiprocessing.Pool.map — draait één parametercombinatie."""
    modus, kp, ki, kd, dt, switch_penalty = args_tuple
    # Water/ff_water: optimaliseer de water PID set; auto/ff_auto: de auto PID set
    if modus in ("water", "ff_water"):
        p = Params(Kp_water=kp, Ki_water=ki, Kd_water=kd)
    else:
        p = Params(Kp=kp, Ki=ki, Kd=kd)
    try:
        r = run_seizoen(modus, p, dt_house_s=dt, verbose=False)
    except Exception as e:
        return dict(kp=kp, ki=ki, kd=kd, kge=-9999.0, score=-9999.0, rmse=99.9, bias=0.0,
                    pct_05=0.0, pct_10=0.0, wisselingen=0, error=str(e))
    kge = r["kge"]
    # Schakelpenalty: λ per 10.000 wisselingen (boven drempel van 20.000/seizoen)
    penalty = switch_penalty * max(0, r["wisselingen"] - 20000) / 10000.0
    score = kge - penalty
    return dict(kp=kp, ki=ki, kd=kd,
                kge=kge, score=score,
                kge_r=r["kge_r"], kge_alpha=r["kge_alpha"], kge_beta=r["kge_beta"],
                rmse=r["rmse"], bias=r["bias"],
                pct_05=r["pct_05"], pct_10=r["pct_10"],
                wisselingen=r["wisselingen"], starts=r["starts"])


# Zoekgrids per modus — gebaseerd op bekende werkgebieden van de controller
GRIDS = {
    # AUTO/FF_AUTO: kamer-tracking. Kp domineert; Ki klein (0.005 schaal); Kd dempt oscillaties
    "auto": {
        "Kp": [10.0, 20.0, 32.0, 50.0, 75.0],
        "Ki": [0.040, 0.150, 0.400, 0.800],
        "Kd": [0.001, 0.010, 0.036, 0.100],
    },
    # WATER/FF_WATER: aanvoer-tracking van variabele stooklijn (28-45°C).
    # Hogere Kp nodig voor tracking van setpoint-wijzigingen bij temperatuurwisselingen
    "water": {
        "Kp": [25.0, 50.0, 80.0, 120.0, 180.0, 250.0],
        "Ki": [0.040, 0.150, 0.400, 0.800],
        "Kd": [0.001, 0.010, 0.036, 0.100],
    },
}


def optimaliseer_pid(modus: str, dt: float = 30.0, workers: int = 4,
                     switch_penalty: float = 0.02) -> dict:
    """
    Grid-search over Kp/Ki/Kd voor het volledige NL stookseizoen.

    modus:          'auto' of 'water' (grids gelden ook voor ff_auto / ff_water)
    dt:             tijdstap huismodel in seconden (30s = snelle run, 10s = nauwkeuriger)
    workers:        aantal parallelle processen
    switch_penalty: λ per 10.000 wisselingen boven drempel van 20.000/seizoen
                    Score = KGE - λ × max(0, wisselingen-20000)/10000
                    0.02 → 10.000 extra wisselingen kost 0.02 KGE-punten
    """
    import itertools
    from multiprocessing import Pool

    grid_key = "water" if modus in ("water", "ff_water") else "auto"
    grid = GRIDS[grid_key]
    keys = list(grid.keys())
    combos = list(itertools.product(*[grid[k] for k in keys]))
    n = len(combos)

    print(f"\n{'═'*80}")
    print(f"  PID OPTIMALISATIE — modus={modus}, {n} combinaties, dt={dt:.0f}s, workers={workers}")
    print(f"  Kp={grid['Kp']}")
    print(f"  Ki={grid['Ki']}")
    print(f"  Kd={grid['Kd']}")
    print(f"  Schakelpenalty: λ={switch_penalty} per 10.000 wisselingen (drempel: 20.000/seizoen)")
    print(f"{'═'*80}")

    work = [(modus, kp, ki, kd, dt, switch_penalty) for (kp, ki, kd) in combos]

    with Pool(processes=workers) as pool:
        results = pool.map(_run_combo, work)

    # Sorteer op gecombineerde score (KGE − penalty), hoger = beter
    results.sort(key=lambda r: r["score"], reverse=True)

    print(f"\n  {'Rang':>4}  {'Kp':>6}  {'Ki':>7}  {'Kd':>7}  "
          f"{'Score':>7}  {'KGE':>7}  {'α':>6}  {'β':>7}  "
          f"{'RMSE':>7}  {'bias':>7}  {'#wiss':>6}  {'#start':>6}")
    print(f"  {'─'*98}")
    for i, r in enumerate(results[:15], 1):
        marker = " ← BEST" if i == 1 else ""
        print(f"  {i:4d}  {r['kp']:6.1f}  {r['ki']:7.4f}  {r['kd']:7.4f}  "
              f"{r['score']:7.4f}  {r['kge']:7.4f}  {r.get('kge_alpha',0):6.3f}  "
              f"{r.get('kge_beta',0):7.4f}  "
              f"{r['rmse']:7.3f}°C  {r['bias']:+7.3f}°C  "
              f"{r['wisselingen']:6d}  {r.get('starts',0):6d}{marker}")

    best = results[0]
    is_water_m = modus in ("water", "ff_water")
    kp_topic = "kp_water" if is_water_m else "kp"
    ki_topic = "ki_water" if is_water_m else "ki"
    kd_topic = "kd_water" if is_water_m else "kd"
    print(f"\n  {'═'*90}")
    print(f"  Beste PID voor modus={modus} (Score={best['score']:.4f}, KGE={best['kge']:.4f}):")
    print(f"    Kp={best['kp']:.1f}  Ki={best['ki']:.4f}  Kd={best['kd']:.4f}")
    print(f"    RMSE={best['rmse']:.3f}°C  bias={best['bias']:+.3f}°C  "
          f"wisselingen={best['wisselingen']}  starts={best.get('starts',0)}")
    print(f"\n  MQTT instellen:")
    print(f"    mosquitto_pub -t chofu/cmd/{kp_topic} -m '{best['kp']:.1f}'")
    print(f"    mosquitto_pub -t chofu/cmd/{ki_topic} -m '{best['ki']:.4f}'")
    print(f"    mosquitto_pub -t chofu/cmd/{kd_topic} -m '{best['kd']:.4f}'")

    return best


# ═══════════════════════════════════════════════════════════════════════════════
#  VERGELIJKING ALLE MODI
# ═══════════════════════════════════════════════════════════════════════════════
def vergelijk_modi(params: Params, dt: float = 5.0, csv_dir: str = "") -> None:
    """Draait alle vier modi en drukt een samenvattende vergelijkingstabel af."""
    modi = ["ff_auto", "auto", "ff_water", "water"]
    resultaten = []

    for modus in modi:
        p = copy.deepcopy(params)
        csv_path = os.path.join(csv_dir, f"seizoen_{modus}.csv") if csv_dir else ""
        r = run_seizoen(modus, p, dt_house_s=dt, csv_path=csv_path, verbose=True)
        resultaten.append(r)

    # Vergelijkingstabel
    print(f"\n\n{'═'*105}")
    print(f"  VERGELIJKING STOOKSEIZOEN — alle modi")
    print(f"{'═'*105}")
    print(f"  {'Modus':10}  {'KGE':>7}  {'r':>6}  {'α':>6}  {'β':>7}  "
          f"{'RMSE':>7}  {'bias':>7}  {'±0.5°C':>7}  {'kWh_el':>8}  {'COP':>5}  "
          f"{'#wiss':>6}  {'#start':>6}")
    print(f"  {'─'*101}")
    for r in resultaten:
        print(f"  {r['modus']:10}  {r['kge']:7.4f}  {r['kge_r']:6.3f}  "
              f"{r['kge_alpha']:6.3f}  {r['kge_beta']:7.4f}  "
              f"{r['rmse']:7.3f}°C  {r['bias']:+7.3f}°C  "
              f"{r['pct_05']:6.1f}%  {r['elec_kwh']:8.1f}  {r['gem_cop']:5.2f}  "
              f"{r['wisselingen']:6d}  {r.get('starts',0):6d}")
    print(f"  {'─'*101}")
    print(f"\n  KGE_const = 1 - RMSE/σ_ref  (σ_ref=1°C, alleen voor auto/ff_auto)")
    print(f"  water/ff_water: standaard KGE (variabele stooklijn als referentie)")
    print(f"  auto/ff_auto: sim=t_kamer vs obs=t_kamer_gewenst")
    print(f"  water/ff_water: sim=t_supply vs obs=stooklijn(t_buiten)")


# ═══════════════════════════════════════════════════════════════════════════════
#  CLI
# ═══════════════════════════════════════════════════════════════════════════════
def main():
    ap = argparse.ArgumentParser(
        description="NL stookseizoen simulatie (1 okt – 30 apr) voor alle WP-modi")
    ap.add_argument("--modus", default="alle",
                    choices=["alle", "ff_auto", "auto", "ff_water", "water"],
                    help="Modus om te simuleren (default: alle)")
    ap.add_argument("--dt",     default=5.0, type=float,
                    help="Huismodel tijdstap in seconden (default: 5, optimise default: 30)")
    ap.add_argument("--csv",    default="",
                    help="CSV-pad (--modus alle: directory, bijv. 'results/')")
    ap.add_argument("--jaar-var", default=1.0, type=float,
                    help="Seizoensamplitude schaalfactor (1.0=normaal, 1.2=koud jaar)")
    ap.add_argument("--optimise", action="store_true",
                    help="PID grid-search voor --modus (auto/water/ff_auto/ff_water)")
    ap.add_argument("--workers", default=4, type=int,
                    help="Parallelle processen voor --optimise (default: 4)")
    ap.add_argument("--switch-penalty", default=0.02, type=float,
                    help="Schakelpenalty λ per 10.000 wisselingen boven 20.000 (default: 0.02)")
    ap.add_argument("--kp",   type=float, help="Kp overschrijven")
    ap.add_argument("--ki",   type=float, help="Ki overschrijven")
    ap.add_argument("--kd",   type=float, help="Kd overschrijven")
    ap.add_argument("--ua-house",   type=float)
    ap.add_argument("--ua-emitter", type=float)
    args = ap.parse_args()

    params = Params()
    if args.kp:         params.Kp            = args.kp
    if args.ki:         params.Ki            = args.ki
    if args.kd:         params.Kd            = args.kd
    if args.ua_house:   params.ff_ua_house   = args.ua_house
    if args.ua_emitter: params.ff_ua_emitter = args.ua_emitter

    if args.optimise:
        modus = args.modus if args.modus != "alle" else "auto"
        dt    = args.dt if args.dt != 5.0 else 30.0   # snellere default voor optimise
        optimaliseer_pid(modus, dt=dt, workers=args.workers,
                         switch_penalty=args.switch_penalty)
    elif args.modus == "alle":
        csv_dir = args.csv
        if csv_dir:
            os.makedirs(csv_dir, exist_ok=True)
        vergelijk_modi(params, dt=args.dt, csv_dir=csv_dir)
    else:
        p = copy.deepcopy(params)
        run_seizoen(args.modus, p, dt_house_s=args.dt,
                    csv_path=args.csv, verbose=True, jaar_var=args.jaar_var)


if __name__ == "__main__":
    main()
