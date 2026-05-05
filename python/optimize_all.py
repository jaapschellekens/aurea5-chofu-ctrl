#!/usr/bin/env python3
"""
Optimaliseer PID en FF controllers voor auto- en water-modus.
Produceert een overzichtstabel met geoptimaliseerde parameters, KGE en RMSE.

Objectieven (KGE, maximaliseren → 1-KGE minimaliseren):
  auto  modus → KGE( chofu/kamer  vs  chofu/kamer_gewenst )  (setpoint-volgfout)
  water modus → KGE( chofu/supply vs  historisch/aanvoer )   (vs OpenTherm boiler)

Gebruik:
    python optimize_all.py
"""
import os, sys
sys.path.insert(0, os.path.dirname(__file__))

import numpy as np
from scipy.optimize import differential_evolution
import pandas as pd

from replay_simulation import (
    load_csv, HouseModel, ArduinoPID, FeedforwardController,
    run_replay,
    HYST_SLOW_PROD, HYST_FAST_PROD, HYST_DOWN_PROD, PID_INT_PROD,
)

# ── Configuratie ──────────────────────────────────────────────────────────────
CSV_FILE   = "./history(4).csv"
START_DATE = "2025-12-01"
END_DATE   = "2026-03-01"   # 3 maanden solide winterdata

MODEL_KW = dict(
    UA_house=263.0, C_th=12.5e6, UA_emitter=344.0,   # gekalibreerd KGE nov25-feb26
    C_water=200e3, COP_eta=0.40,
    t_kamer_init=20.0, t_supply_init=35.0,
)

HYST_KW = dict(
    hyst_slow_s   = HYST_SLOW_PROD / 1000,
    hyst_fast_s   = HYST_FAST_PROD / 1000,
    hyst_down_s   = HYST_DOWN_PROD / 1000,
    pid_interval_s= PID_INT_PROD   / 1000,
)

G_FLOW    = 600.0
UA_em_eff = (2.0 * G_FLOW * MODEL_KW["UA_emitter"]
             / (2.0 * G_FLOW + MODEL_KW["UA_emitter"]))


# ── KGE hulpfunctie ───────────────────────────────────────────────────────────
def kge_loss(sim_arr, obs_arr):
    """1 − KGE (minimaliseren; 0 = perfect)."""
    if len(obs_arr) < 10 or obs_arr.std() < 1e-6:
        return 2.0
    r = np.corrcoef(sim_arr, obs_arr)[0, 1]
    if not np.isfinite(r):
        return 2.0
    alpha = sim_arr.std() / obs_arr.std()
    beta  = sim_arr.mean() / obs_arr.mean() if abs(obs_arr.mean()) > 1e-6 else 1.0
    return float(1.0 - (1.0 - np.sqrt((r-1)**2 + (alpha-1)**2 + (beta-1)**2)))


# ── Score- en RMSE-helpers ────────────────────────────────────────────────────
def _extract(sim, ref_series, sim_col, ref_col):
    """Geeft (sim_arr, obs_arr) terug, of (None, None) bij ontbrekende data."""
    if sim.empty or sim_col not in sim.columns:
        return None, None
    if ref_series is not None and ref_col is not None and ref_col in ref_series.columns:
        merged = sim[[sim_col]].join(ref_series[[ref_col]], how="inner").dropna()
        if len(merged) < 10:
            return None, None
        return merged[sim_col].values, merged[ref_col].values
    if "chofu/kamer_gewenst" in sim.columns:
        merged = sim[[sim_col, "chofu/kamer_gewenst"]].dropna()
        if len(merged) < 10:
            return None, None
        return merged[sim_col].values, merged["chofu/kamer_gewenst"].values
    return None, None


def _score(sim, ref_series, sim_col, ref_col):
    """1-KGE voor optimalisatie."""
    s, o = _extract(sim, ref_series, sim_col, ref_col)
    if s is None:
        return 2.0
    return kge_loss(s, o)


def _rmse(sim, ref_series, sim_col, ref_col):
    """RMSE voor rapportage."""
    s, o = _extract(sim, ref_series, sim_col, ref_col)
    if s is None:
        return float("nan")
    return float(np.sqrt(np.mean((s - o)**2)))


# ── Statistieken uit simulatieresultaat ───────────────────────────────────────
def sim_stats(sim_df):
    if sim_df.empty or "chofu/stand" not in sim_df.columns:
        return 0, 0
    stand = sim_df["chofu/stand"]
    starts   = int(((stand > 0) & (stand.shift(1) == 0)).sum())
    wijzigen = int((stand.diff().abs() > 0).sum())
    return starts, wijzigen


def run_with_params(params, ctrl_type, pivot, ref, modus, kamer_sp):
    """Evalueer parameters; geeft (kge, rmse, starts, wijzigingen)."""
    model = HouseModel(**MODEL_KW)
    if ctrl_type == "pid":
        Kp, Ki, Kd = params
        ctrl = ArduinoPID(modus=modus, Kp=Kp, Ki=Ki, Kd=Kd, **HYST_KW)
    else:
        ua_house, ki_kamer, coast_k = params
        ctrl = FeedforwardController(
            modus=modus, UA_house=ua_house, Ki_kamer=ki_kamer,
            ua_learn_rate=0.002, kamer_coast_k=coast_k,
            max_stand_stap=1, UA_emitter_eff=UA_em_eff,
            **HYST_KW,
        )
    sim = run_replay(pivot, model, ctrl, modus=modus,
                     kamer_sp=kamer_sp, verbose=False)
    if modus == "water":
        kge  = 1.0 - _score(sim, ref, "chofu/supply", "historisch/aanvoer")
        rmse = _rmse(sim, ref, "chofu/supply", "historisch/aanvoer")
    else:
        kge  = 1.0 - _score(sim, None, "chofu/kamer", None)
        rmse = _rmse(sim, None, "chofu/kamer", None)
    starts, wijzigen = sim_stats(sim)
    return kge, rmse, starts, wijzigen


# ── Objectieffuncties ─────────────────────────────────────────────────────────
def score_pid(params, pivot, ref, modus, kamer_sp):
    Kp, Ki, Kd = params
    model = HouseModel(**MODEL_KW)
    ctrl  = ArduinoPID(modus=modus, Kp=Kp, Ki=Ki, Kd=Kd, **HYST_KW)
    sim   = run_replay(pivot, model, ctrl, modus=modus,
                       kamer_sp=kamer_sp, verbose=False)
    if modus == "water":
        return _score(sim, ref, "chofu/supply", "historisch/aanvoer")
    return _score(sim, None, "chofu/kamer", None)


def score_ff(params, pivot, ref, modus, kamer_sp):
    ua_house, ki_kamer, coast_k = params
    model = HouseModel(**MODEL_KW)
    ctrl  = FeedforwardController(
        modus=modus, UA_house=ua_house, Ki_kamer=ki_kamer,
        ua_learn_rate=0.002, kamer_coast_k=coast_k,
        max_stand_stap=1, UA_emitter_eff=UA_em_eff,
        **HYST_KW,
    )
    sim = run_replay(pivot, model, ctrl, modus=modus,
                     kamer_sp=kamer_sp, verbose=False)
    if modus == "water":
        return _score(sim, ref, "chofu/supply", "historisch/aanvoer")
    return _score(sim, None, "chofu/kamer", None)


# ── Optimizer wrapper ─────────────────────────────────────────────────────────
def optimize(name, obj_fn, bounds, pivot, ref, modus, kamer_sp,
             popsize=10, maxiter=60):
    print(f"\n{'═'*64}")
    print(f"  {name}")
    print(f"{'═'*64}")
    n = [0]; best = [2.0]
    param_names = [b[0] for b in bounds]
    bounds_vals = [b[1] for b in bounds]

    def wrapped(params):
        val = obj_fn(params, pivot, ref, modus, kamer_sp)
        n[0] += 1
        if val < best[0]:
            best[0] = val
            pstr = "  ".join(f"{nm}={v:.4g}" for nm, v in zip(param_names, params))
            print(f"  [{n[0]:4d}] {pstr}  → KGE={1-val:.3f}  ★")
        elif n[0] % 50 == 0:
            print(f"  [{n[0]:4d}] beste KGE: {1-best[0]:.3f}")
        return val

    result = differential_evolution(
        wrapped, bounds_vals,
        maxiter=maxiter, popsize=popsize, tol=0.002,
        seed=42, polish=True, workers=1,
    )
    print(f"\n  Klaar: KGE={1-result.fun:.3f}  ({n[0]} evaluaties)")
    return result.x, 1.0 - result.fun, n[0]


# ── Main ───────────────────────────────────────────────────────────────────────
def main():
    print(f"Data laden: {START_DATE} → {END_DATE}  ({CSV_FILE})")
    pivot, ref, _ = load_csv(CSV_FILE, START_DATE, END_DATE)
    if pivot.empty:
        print("Geen data!")
        sys.exit(1)

    kamer_sp = (float(pivot["anna/setpoint"].mean())
                if "anna/setpoint" in pivot.columns else 20.0)
    print(f"Kamer setpoint: {kamer_sp:.1f}°C (gemiddelde anna/setpoint)")

    rows = []

    # ── 1. PID auto ───────────────────────────────────────────────────────────
    params, kge, n = optimize(
        "PID  —  auto modus  (KGE kamer vs kamer_gewenst)",
        score_pid,
        bounds=[("Kp",(0.1,20)), ("Ki",(0.001,0.3)), ("Kd",(0.0,1.5))],
        pivot=pivot, ref=ref, modus="auto", kamer_sp=kamer_sp,
    )
    kge, rmse, starts, wijz = run_with_params(params, "pid", pivot, ref, "auto", kamer_sp)
    rows.append({"Modus":"auto", "Controller":"PID",
                 "Kp":round(params[0],3), "Ki":round(params[1],5), "Kd":round(params[2],3),
                 "UA_house":"-", "Ki_ff":"-", "coast_k":"-",
                 "KGE":round(kge,3), "RMSE (°C)":round(rmse,3),
                 "Starts":starts, "Wijzigingen":wijz})

    # ── 2. PID water ──────────────────────────────────────────────────────────
    params, kge, n = optimize(
        "PID  —  water modus  (KGE aanvoer vs OpenTherm historisch)",
        score_pid,
        bounds=[("Kp",(0.1,20)), ("Ki",(0.001,0.3)), ("Kd",(0.0,1.5))],
        pivot=pivot, ref=ref, modus="water", kamer_sp=kamer_sp,
    )
    kge, rmse, starts, wijz = run_with_params(params, "pid", pivot, ref, "water", kamer_sp)
    rows.append({"Modus":"water", "Controller":"PID",
                 "Kp":round(params[0],3), "Ki":round(params[1],5), "Kd":round(params[2],3),
                 "UA_house":"-", "Ki_ff":"-", "coast_k":"-",
                 "KGE":round(kge,3), "RMSE (°C)":round(rmse,3),
                 "Starts":starts, "Wijzigingen":wijz})

    # ── 3. FF auto ────────────────────────────────────────────────────────────
    params, kge, n = optimize(
        "FF   —  auto modus  (KGE kamer vs kamer_gewenst)",
        score_ff,
        bounds=[("UA_house",(50,350)), ("Ki_kamer",(0.0001,0.05)),
                ("coast_k",(0.5,5.0))],
        pivot=pivot, ref=ref, modus="auto", kamer_sp=kamer_sp,
    )
    kge, rmse, starts, wijz = run_with_params(params, "ff", pivot, ref, "auto", kamer_sp)
    rows.append({"Modus":"auto", "Controller":"FF",
                 "Kp":"-", "Ki":"-", "Kd":"-",
                 "UA_house":round(params[0],1), "Ki_ff":round(params[1],5),
                 "coast_k":round(params[2],2),
                 "KGE":round(kge,3), "RMSE (°C)":round(rmse,3),
                 "Starts":starts, "Wijzigingen":wijz})

    # ── 4. FF water ───────────────────────────────────────────────────────────
    params, kge, n = optimize(
        "FF   —  water modus  (KGE aanvoer vs OpenTherm historisch)",
        score_ff,
        bounds=[("UA_house",(50,350)), ("Ki_kamer",(0.0001,0.05)),
                ("coast_k",(0.5,5.0))],
        pivot=pivot, ref=ref, modus="water", kamer_sp=kamer_sp,
    )
    kge, rmse, starts, wijz = run_with_params(params, "ff", pivot, ref, "water", kamer_sp)
    rows.append({"Modus":"water", "Controller":"FF",
                 "Kp":"-", "Ki":"-", "Kd":"-",
                 "UA_house":round(params[0],1), "Ki_ff":round(params[1],5),
                 "coast_k":round(params[2],2),
                 "KGE":round(kge,3), "RMSE (°C)":round(rmse,3),
                 "Starts":starts, "Wijzigingen":wijz})

    # ── Tabel ─────────────────────────────────────────────────────────────────
    df = pd.DataFrame(rows, columns=[
        "Modus","Controller","Kp","Ki","Kd","UA_house","Ki_ff","coast_k",
        "KGE","RMSE (°C)","Starts","Wijzigingen"
    ])

    print(f"\n\n{'═'*72}")
    print("  GEOPTIMALISEERDE PARAMETERS  —  dec 2025 t/m feb 2026")
    print(f"{'═'*72}")
    print(df.to_string(index=False))
    print(f"{'═'*72}")
    print()
    print("Noten:")
    print("  auto  KGE/RMSE = kamertemperatuur vs kamer_gewenst (simulatie-intern)")
    print("  water KGE/RMSE = aanvoertemperatuur vs historische OpenTherm aanvoer")
    print("  FF UA_house = startwaarde; controller leert deze online bij")
    print("  Starts      = aantal keer WP aangaat (stand 0→>0)")
    print("  Wijzigingen = totaal aantal stand-veranderingen")

    os.makedirs("sim_output", exist_ok=True)
    df.to_csv("sim_output/optimalisatie_resultaten.csv", index=False)
    print(f"\nTabel opgeslagen: sim_output/optimalisatie_resultaten.csv")


if __name__ == "__main__":
    main()
