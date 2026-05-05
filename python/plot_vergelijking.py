#!/usr/bin/env python3
"""
Vergelijkingsgrafieken: PID default / PID optimaal / FF controller
voor auto modus en water modus.
"""
import glob
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.dates as mdates
import numpy as np
import pandas as pd

OUTPUT_DIR = "sim_output"

# ── Bestanden laden ────────────────────────────────────────────────────────────
def last(d):
    fls = sorted(glob.glob(f"sim_output/{d}/sim_results_*.csv"))
    return fls[-1] if fls else None

def second_last(d):
    fls = sorted(glob.glob(f"sim_output/{d}/sim_results_*.csv"))
    return fls[-2] if len(fls) >= 2 else None

runs = {
    "auto": [
        ("PID default\n(Kp=0.8 Ki=0.01 Kd=0.3)",  "#e74c3c", second_last("pid_auto")),
        ("PID optimaal\n(Kp=18.96 Ki=0.17 Kd=0.01)", "#e67e22", last("pid_auto")),
        ("FF controller\n(UA-leren + stooklijn)",     "#2980b9", last("ff_auto")),
    ],
    "water": [
        ("PID default\n(Kp=0.8 Ki=0.01 Kd=0.3)",  "#e74c3c", second_last("pid_water")),
        ("PID optimaal\n(Kp=4.65 Ki=0.29 Kd=0.0)", "#e67e22", last("pid_water")),
        ("FF controller\n(stooklijn eigen)",          "#2980b9", last("ff_water")),
    ],
}

def load(f):
    if not f or not os.path.exists(f):
        return None
    return pd.read_csv(f, index_col=0, parse_dates=True)

def fmt_ax(ax):
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%d/%m"))
    ax.xaxis.set_major_locator(mdates.DayLocator())
    ax.tick_params(axis="x", rotation=30, labelsize=7)
    ax.grid(True, alpha=0.25)

# ── Laad referentie (historisch) uit een van de bestanden ─────────────────────
ref_auto  = load(last("ff_auto"))
ref_water = load(last("ff_water"))


# ══════════════════════════════════════════════════════════════════════════════
# GRAFIEK 1 — Auto modus: temperaturen
# ══════════════════════════════════════════════════════════════════════════════
fig, axes = plt.subplots(3, 1, figsize=(14, 11), sharex=True)
fig.suptitle("Vergelijking regelaars — AUTO modus  (10–20 feb)", fontsize=12, fontweight="bold")

# Subplot 1: kamertemperatuur alle controllers
ax = axes[0]
for label, color, f in runs["auto"]:
    df = load(f)
    if df is None:
        continue
    ax.plot(df.index, df["chofu/kamer"], label=label.replace("\n", " "),
            color=color, lw=1.2, alpha=0.85)
# Setpoint (van ff_auto als voorbeeld)
if ref_auto is not None:
    ax.plot(ref_auto.index, ref_auto["chofu/kamer_gewenst"],
            label="Setpoint", color="black", lw=1, linestyle="--", alpha=0.6)
ax.set_ylabel("°C")
ax.set_title("Kamertemperatuur")
ax.legend(fontsize=7, ncol=4)
fmt_ax(ax)

# Subplot 2: stand alle controllers
ax = axes[1]
offsets = [-0.15, 0, 0.15]
for (label, color, f), off in zip(runs["auto"], offsets):
    df = load(f)
    if df is None:
        continue
    ax.step(df.index, df["chofu/stand"].clip(0, 12) + off,
            label=label.replace("\n", " "), color=color, lw=0.9, alpha=0.75, where="post")
ax.set_ylabel("Stand (0–8)")
ax.set_ylim(-0.5, 9)
ax.set_title("Compressorstand")
ax.legend(fontsize=7, ncol=3)
fmt_ax(ax)

# Subplot 3: fout (kamer − setpoint) als gestreept vlak per controller
ax = axes[2]
for label, color, f in runs["auto"]:
    df = load(f)
    if df is None:
        continue
    fout = df["chofu/kamer"] - df["chofu/kamer_gewenst"]
    ax.plot(df.index, fout, label=label.replace("\n", " "), color=color, lw=0.9, alpha=0.75)
ax.axhline(0, color="black", lw=0.8, linestyle="--")
ax.axhline( 0.5, color="gray", lw=0.5, linestyle=":")
ax.axhline(-0.5, color="gray", lw=0.5, linestyle=":")
ax.set_ylabel("Fout (°C)")
ax.set_title("Kamerfout  (positief = te warm)")
ax.legend(fontsize=7, ncol=3)
fmt_ax(ax)

fig.tight_layout()
p = os.path.join(OUTPUT_DIR, "vergelijking_auto.png")
fig.savefig(p, dpi=150)
plt.close(fig)
print(f"Grafiek: {p}")


# ══════════════════════════════════════════════════════════════════════════════
# GRAFIEK 2 — Water modus: temperaturen
# ══════════════════════════════════════════════════════════════════════════════
fig, axes = plt.subplots(3, 1, figsize=(14, 11), sharex=True)
fig.suptitle("Vergelijking regelaars — WATER modus  (10–20 feb)", fontsize=12, fontweight="bold")

ax = axes[0]
for label, color, f in runs["water"]:
    df = load(f)
    if df is None:
        continue
    ax.plot(df.index, df["chofu/supply"], label=label.replace("\n", " "),
            color=color, lw=1.2, alpha=0.85)
if ref_water is not None:
    ax.plot(ref_water.index, ref_water["chofu/water_setpoint"],
            label="Stooklijn setpoint", color="black", lw=1.2, linestyle="--", alpha=0.7)
ax.set_ylabel("°C")
ax.set_title("Aanvoertemperatuur")
ax.legend(fontsize=7, ncol=4)
fmt_ax(ax)

ax = axes[1]
for (label, color, f), off in zip(runs["water"], offsets):
    df = load(f)
    if df is None:
        continue
    ax.step(df.index, df["chofu/stand"].clip(0, 12) + off,
            label=label.replace("\n", " "), color=color, lw=0.9, alpha=0.75, where="post")
ax.set_ylabel("Stand (0–8)")
ax.set_ylim(-0.5, 9)
ax.set_title("Compressorstand")
ax.legend(fontsize=7, ncol=3)
fmt_ax(ax)

ax = axes[2]
for label, color, f in runs["water"]:
    df = load(f)
    if df is None:
        continue
    fout = df["chofu/supply"] - df["chofu/water_setpoint"]
    ax.plot(df.index, fout, label=label.replace("\n", " "), color=color, lw=0.9, alpha=0.75)
ax.axhline(0,  color="black", lw=0.8, linestyle="--")
ax.axhline( 2, color="gray",  lw=0.5, linestyle=":")
ax.axhline(-2, color="gray",  lw=0.5, linestyle=":")
ax.set_ylabel("Fout (°C)")
ax.set_title("Aanvoerfout t.o.v. stooklijn  (positief = te warm)")
ax.legend(fontsize=7, ncol=3)
fmt_ax(ax)

fig.tight_layout()
p = os.path.join(OUTPUT_DIR, "vergelijking_water.png")
fig.savefig(p, dpi=150)
plt.close(fig)
print(f"Grafiek: {p}")


# ══════════════════════════════════════════════════════════════════════════════
# GRAFIEK 3 — Statistieken: boxplots + staafdiagrammen
# ══════════════════════════════════════════════════════════════════════════════
fig, axes = plt.subplots(2, 2, figsize=(13, 8))
fig.suptitle("Statistieken — PID default / PID optimaal / FF", fontsize=12, fontweight="bold")

labels_short = ["PID\ndefault", "PID\nopt.", "FF"]
colors_bar   = ["#e74c3c", "#e67e22", "#2980b9"]

for col, (modus, runs_m) in enumerate(runs.items()):
    # Boxplot fouten
    ax = axes[0][col]
    data, clrs = [], []
    for label, color, f in runs_m:
        df = load(f)
        if df is None:
            data.append([])
            clrs.append(color)
            continue
        if modus == "water":
            fout = (df["chofu/supply"] - df["chofu/water_setpoint"]).dropna().values
        else:
            fout = (df["chofu/kamer"] - df["chofu/kamer_gewenst"]).dropna().values
        data.append(fout)
        clrs.append(color)

    bp = ax.boxplot(data, tick_labels=labels_short, patch_artist=True,
                    medianprops=dict(color="black", lw=2),
                    whiskerprops=dict(lw=1), capprops=dict(lw=1))
    for patch, c in zip(bp["boxes"], clrs):
        patch.set_facecolor(c)
        patch.set_alpha(0.6)
    ax.axhline(0, color="black", lw=0.8, linestyle="--")
    ax.set_ylabel("Fout (°C)")
    ax.set_title(f"{modus.capitalize()} modus — fout-verdeling")
    ax.grid(True, alpha=0.25, axis="y")

    # Staafdiagram stand-wijzigingen
    ax = axes[1][col]
    wijz_vals = []
    for label, color, f in runs_m:
        df = load(f)
        if df is None:
            wijz_vals.append(0)
            continue
        stand = df["chofu/stand"].clip(0, 12)
        wijz_vals.append((stand.diff().abs() > 0).sum())
    bars = ax.bar(labels_short, wijz_vals, color=colors_bar, alpha=0.7, edgecolor="white")
    for bar, v in zip(bars, wijz_vals):
        ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 15,
                str(v), ha="center", va="bottom", fontsize=9, fontweight="bold")
    ax.set_ylabel("Aantal stand-wijzigingen")
    ax.set_title(f"{modus.capitalize()} modus — stand-activiteit")
    ax.grid(True, alpha=0.25, axis="y")

fig.tight_layout()
p = os.path.join(OUTPUT_DIR, "vergelijking_statistieken.png")
fig.savefig(p, dpi=150)
plt.close(fig)
print(f"Grafiek: {p}")

print("\nAlle vergelijkingsgrafieken opgeslagen.")
