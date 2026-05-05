#!/usr/bin/env python3
"""
Kalibreer HouseModel parameters op historische data.

Aanpak — open-loop integratie:
  Historische T_aanvoer + T_buiten worden als INVOER gebruikt (niet gesimuleerd).
  De optimizer past UA_house, C_th en UA_emitter aan zodat de gesimuleerde
  kamertemperatuur zo goed mogelijk de gemeten kamertemperatuur volgt.

Objectief: KGE (Kling-Gupta Efficiency), 1 − KGE geminimaliseerd.
  KGE = 1 − √[(r−1)² + (α−1)² + (β−1)²]
    r : Pearson correlatie       (timing/fase)
    α : σ_sim / σ_obs            (variabiliteit — voorkomt degeneratie naar vlak model)
    β : μ_sim / μ_obs            (gemiddelde bias)

Gebruik:
    python calibrate_housemodel.py
"""
import os, sys
sys.path.insert(0, os.path.dirname(__file__))

import numpy as np
from scipy.optimize import differential_evolution
import pandas as pd

from replay_simulation import load_csv

CSV_FILE  = "./history(4).csv"
CAL_START = "2025-11-01"   # begin stookseizoen
CAL_END   = "2026-03-01"   # voor lente / zonnewinst-periode

G_FLOW    = 600.0          # W/K  (m_dot × Cp afgifte-circuit)


# ── KGE hulpfunctie ───────────────────────────────────────────────────────────
def kge_loss(sim, obs):
    """
    Geeft 1 − KGE terug (te minimaliseren; 0 is perfect).
    Straft een vlak model af via de variabiliteitsterm α = σ_sim/σ_obs.
    """
    if obs.std() < 1e-6:
        return 2.0
    r     = np.corrcoef(sim, obs)[0, 1]
    if not np.isfinite(r):
        return 2.0
    alpha = sim.std() / obs.std()
    beta  = sim.mean() / obs.mean() if abs(obs.mean()) > 1e-6 else 1.0
    kge   = 1.0 - np.sqrt((r - 1.0)**2 + (alpha - 1.0)**2 + (beta - 1.0)**2)
    return float(1.0 - kge)


# ── Open-loop huismodel ───────────────────────────────────────────────────────
def simulate_open_loop(t_supply_arr, t_outside_arr, dt_s,
                       UA_house, C_th, UA_emitter, t_kamer_init):
    """
    Integreer kamertemperatuur met historische aanvoer- en buitentemperatuur
    als forcing. Geen WP-model nodig.

    Energiebalans kamer:
      C_th · dT_kamer/dt = P_afgegeven − P_verlies
      P_afgegeven = UA_emitter_eff · (T_aanvoer − T_kamer)   [alleen als positief]
      P_verlies   = UA_house       · (T_kamer  − T_buiten)
    """
    ua_eff  = 2.0 * G_FLOW * UA_emitter / (2.0 * G_FLOW + UA_emitter)
    t_kamer = t_kamer_init
    results = np.empty(len(t_supply_arr))
    results[0] = t_kamer

    for i in range(1, len(t_supply_arr)):
        p_af   = max(0.0, ua_eff  * (t_supply_arr[i-1]  - t_kamer))
        p_verl = UA_house * (t_kamer - t_outside_arr[i-1])
        t_kamer += (p_af - p_verl) / C_th * dt_s
        results[i] = t_kamer

    return results


# ── Data laden ────────────────────────────────────────────────────────────────
def load_cal(csv_file, start, end):
    pivot, ref, _ = load_csv(csv_file, start, end)

    combined = pd.DataFrame({
        "t_kamer":   ref.get("historisch/kamer"),
        "t_aanvoer": ref.get("historisch/aanvoer"),
        "t_buiten":  pivot.get("chofu/sim/outside"),
    }).dropna()

    if len(combined) < 100:
        print(f"Te weinig overlappende data: {len(combined)} punten.")
        sys.exit(1)

    dt_s = float(combined.index.to_series().diff().median().total_seconds())
    print(f"Kalibratieset: {len(combined)} punten  "
          f"({combined.index[0].date()} → {combined.index[-1].date()})  "
          f"dt={dt_s/60:.0f} min")
    print(f"  T_buiten:  {combined['t_buiten'].mean():.1f}°C gem  "
          f"(min {combined['t_buiten'].min():.1f}  max {combined['t_buiten'].max():.1f})")
    print(f"  T_kamer:   {combined['t_kamer'].mean():.1f}°C gem  "
          f"(min {combined['t_kamer'].min():.1f}  max {combined['t_kamer'].max():.1f})")
    print(f"  T_aanvoer: {combined['t_aanvoer'].mean():.1f}°C gem  "
          f"(min {combined['t_aanvoer'].min():.1f}  max {combined['t_aanvoer'].max():.1f})")

    return (combined["t_aanvoer"].values,
            combined["t_buiten"].values,
            combined["t_kamer"].values,
            dt_s,
            float(combined["t_kamer"].iloc[0]))


# ── Optimalisatie ─────────────────────────────────────────────────────────────
def main():
    print(f"HouseModel kalibratie  —  {CAL_START} → {CAL_END}\n")

    t_supply_arr, t_buiten_arr, t_kamer_ref, dt_s, t_kamer_init = \
        load_cal(CSV_FILE, CAL_START, CAL_END)

    n_eval = [0]
    best   = [2.0]   # 1-KGE, laagste wint

    def objective(params):
        UA_house, log_Cth, UA_emitter = params
        C_th = 10.0 ** log_Cth
        sim  = simulate_open_loop(t_supply_arr, t_buiten_arr, dt_s,
                                   UA_house, C_th, UA_emitter, t_kamer_init)
        loss = kge_loss(sim, t_kamer_ref)
        n_eval[0] += 1
        if loss < best[0]:
            best[0] = loss
            tau_h = C_th / UA_house / 3600.0
            kge   = 1.0 - loss
            r     = np.corrcoef(sim, t_kamer_ref)[0, 1]
            alpha = sim.std() / t_kamer_ref.std()
            beta  = sim.mean() / t_kamer_ref.mean()
            print(f"  [{n_eval[0]:4d}] UA_house={UA_house:.1f}  "
                  f"C_th={C_th/1e6:.2f} MJ/K (τ={tau_h:.0f}h)  "
                  f"UA_em={UA_emitter:.1f}  "
                  f"→ KGE={kge:.3f}  r={r:.3f}  α={alpha:.3f}  β={beta:.3f}  ★")
        elif n_eval[0] % 100 == 0:
            print(f"  [{n_eval[0]:4d}] beste 1-KGE: {best[0]:.4f}  (KGE={1-best[0]:.3f})")
        return loss

    bounds = [
        (30.0,  300.0),   # UA_house  W/K
        (5.5,   7.5),     # log10(C_th): 3.2–31.6 MJ/K
        (50.0,  600.0),   # UA_emitter W/K
    ]

    print(f"Zoekruimte:")
    print(f"  UA_house   : 30–300 W/K")
    print(f"  C_th       : 3.2–31.6 MJ/K  (log-schaal)")
    print(f"  UA_emitter : 50–600 W/K")
    print(f"\nOptimalisatie met KGE objectief (differential_evolution):\n")

    result = differential_evolution(
        objective, bounds,
        maxiter=200, popsize=15, tol=0.0005,
        seed=42, polish=True, workers=1,
    )

    UA_house_opt, log_Cth_opt, UA_emitter_opt = result.x
    C_th_opt  = 10.0 ** log_Cth_opt
    tau_opt   = C_th_opt / UA_house_opt / 3600.0
    ua_eff    = 2.0 * G_FLOW * UA_emitter_opt / (2.0 * G_FLOW + UA_emitter_opt)
    kge_final = 1.0 - result.fun

    sim_final = simulate_open_loop(t_supply_arr, t_buiten_arr, dt_s,
                                    UA_house_opt, C_th_opt, UA_emitter_opt, t_kamer_init)
    rmse_final = float(np.sqrt(np.mean((sim_final - t_kamer_ref)**2)))
    r_final    = float(np.corrcoef(sim_final, t_kamer_ref)[0, 1])
    alpha_fin  = sim_final.std() / t_kamer_ref.std()
    beta_fin   = sim_final.mean() / t_kamer_ref.mean()

    print(f"\n{'═'*64}")
    print(f"  GEKALIBREERDE HUISMODEL PARAMETERS")
    print(f"{'═'*64}")
    print(f"  UA_house   = {UA_house_opt:.1f} W/K"
          f"   →  ontwerplast @-10°C: {UA_house_opt*30/1000:.1f} kW")
    print(f"  C_th       = {C_th_opt/1e6:.2f} MJ/K"
          f"   →  tijdconstante τ = {tau_opt:.0f} uur")
    print(f"  UA_emitter = {UA_emitter_opt:.1f} W/K"
          f"   →  UA_eff = {ua_eff:.1f} W/K"
          f"   →  vermogen @35°C/20°C: {ua_eff*(35-20)/1000:.1f} kW")
    print(f"  KGE        = {kge_final:.3f}   (r={r_final:.3f}  α={alpha_fin:.3f}  β={beta_fin:.3f})")
    print(f"  RMSE       = {rmse_final:.3f}°C   ({n_eval[0]} evaluaties)")
    print(f"{'═'*64}")

    print(f"""
Gebruik in replay_simulation.py of optimize_all.py:
  UA_house={UA_house_opt:.1f}, C_th={C_th_opt:.0f}, UA_emitter={UA_emitter_opt:.1f}

Kanttekeningen:
  UA_emitter is minder betrouwbaar door multi-zone T_aanvoer (andere zones
  verhogen T_aanvoer ook als de woonkamer-ventiel dicht is).
""")

    os.makedirs("sim_output", exist_ok=True)
    res = pd.DataFrame([{
        "UA_house (W/K)":    round(UA_house_opt, 1),
        "C_th (MJ/K)":       round(C_th_opt/1e6, 3),
        "tau (uur)":          round(tau_opt, 1),
        "UA_emitter (W/K)":  round(UA_emitter_opt, 1),
        "UA_eff (W/K)":      round(ua_eff, 1),
        "KGE":               round(kge_final, 4),
        "RMSE kamer (°C)":   round(rmse_final, 3),
        "periode":           f"{CAL_START} → {CAL_END}",
    }])
    res.to_csv("sim_output/housemodel_kalibratie.csv", index=False)
    print(f"Resultaten opgeslagen: sim_output/housemodel_kalibratie.csv")


if __name__ == "__main__":
    main()
