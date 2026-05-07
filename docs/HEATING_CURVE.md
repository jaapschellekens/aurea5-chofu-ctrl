# Heating Curve (Stooklijn)

The heating curve automatically raises the supply water temperature as the outdoor temperature drops. This compensates for higher heat loss through walls and windows when it gets colder outside, without requiring manual adjustments.

---

## How It Works

The controller calculates a **target supply temperature** (`doel_setpoint`) from the outdoor temperature:

```
doel_setpoint = setpoint + (STOOKLIJN_GRENS − t_outside) × STOOKLIJN_FACTOR
                                ↑ only applied when t_outside < STOOKLIJN_GRENS
```

The result is capped at **45°C**.

### Example with default settings

| Parameter | Default value |
|-----------|--------------|
| `setpoint` (base supply temp) | 28°C |
| `STOOKLIJN_GRENS` (boost threshold) | 15°C |
| `STOOKLIJN_FACTOR` (slope) | 0.68 °C/°C |

| Outdoor temp | Target supply temp |
|---|---|
| 20°C | 28.0°C — no boost, heating off (above `STOOKLIJN_UIT_GRENS`) |
| 15°C | 28.0°C — flat (exactly at threshold) |
| 10°C | 31.4°C |
|  5°C | 34.8°C |
|  0°C | 38.2°C |
| −5°C | 41.6°C |
| −7°C | 43.2°C |
| −10°C | 45.0°C — capped |

---

## Heating Season Boundaries

Two thresholds control when heating runs at all:

### `STOOKLIJN_UIT_GRENS` — Heating shut-off (default: 15°C)

When the outdoor temperature rises **above** this threshold, the heat pump is switched off completely. Heating season is over. Applies to `AUTO` and `FF_AUTO` modes.

### `T_VORST` — Frost protection (default: 4°C)

When the outdoor temperature drops **below** this threshold, the heat pump is forced on at minimum stage (stage 1), even if the room is already warm enough. This prevents pipes from freezing. Applies to `AUTO` and `FF_AUTO` modes.

---

## How Each Control Mode Uses the Heating Curve

### `AUTO` — Room PID

The heating curve provides the **PID setpoint**. The PID controller compares the calculated `doel_setpoint` against the actual supply temperature and adjusts the compressor stage to close the gap.

```
doel_setpoint = heating curve result
aanvoer_fout  = doel_setpoint − t_supply   ← PID error
```

The curve ensures the PID always chases a weather-appropriate target — without it the PID would demand the same supply temperature regardless of outdoor conditions.

---

### `FF_AUTO` — Feedforward on room temperature

The heating curve is **computed and published** to `chofu/doel_setpoint` for monitoring, but it does **not** enter the control calculation. Instead, the feedforward directly computes the required compressor stage from the heat balance:

```
P_needed = UA_house × (t_room_setpoint − t_outside)
COP      = 0.40 × (T_supply_K / (T_supply_K − T_outside_K))
stage    = lowest stage where VERMOGEN[stage] ≥ P_needed / COP
```

The `UA_house` value is learned online, making this mode self-calibrating. The heating curve is not needed because the physics model already accounts for outdoor temperature.

> **Tip:** When using `FF_AUTO`, tune `ff_UA_house` rather than the heating curve parameters.

---

### `WATER` — Supply PID on fixed setpoint

The heating curve is **not used**. The PID controls the supply temperature to a fixed value (`t_water_gewenst`, default 32°C), regardless of outdoor temperature. Set `t_water_gewenst` to whatever the emitter system (underfloor heating, radiators) needs.

---

### `FF_WATER` — Feedforward on supply temperature

The heating curve **replaces** the fixed water setpoint. The computed `doel_setpoint` becomes the FF supply target (`wsp`):

```
wsp = setpoint + (STOOKLIJN_GRENS − t_outside) × STOOKLIJN_FACTOR
```

This gives `FF_WATER` automatic weather compensation: the FF controller tracks a higher supply temperature on colder days, while still learning the emitter's thermal resistance (`UA_emitter`) online.

---

### `HANDMATIG` — Manual stage

Not used. The compressor runs at a fixed stage set via `chofu/cmd/stand`.

---

## Summary Table

| Mode | Uses heating curve? | Role |
|------|---------------------|------|
| `AUTO` | ✅ Yes | PID setpoint (supply temp target) |
| `FF_AUTO` | ℹ️ Computed, not used in control | Monitoring only (`chofu/doel_setpoint`) |
| `WATER` | ❌ No | Fixed `t_water_gewenst` instead |
| `FF_WATER` | ✅ Yes | Replaces fixed water setpoint |
| `HANDMATIG` | ❌ No | — |

---

## Adjusting the Heating Curve

All parameters are **saved to EEPROM** on change — they survive a reboot. Send the value as a plain number (e.g. `0.85`) to the MQTT command topic.

| MQTT command topic | Parameter | Default | Range | What it controls |
|--------------------|-----------|---------|-------|-----------------|
| `chofu/cmd/setpoint` | `setpoint` | 28°C | — | Base supply temp (flat part of the curve, at and above `STOOKLIJN_GRENS`) |
| `chofu/cmd/stooklijn_grens` | `STOOKLIJN_GRENS` | 15°C | 0–25°C | Outdoor temp at which the boost starts |
| `chofu/cmd/stooklijn_factor` | `STOOKLIJN_FACTOR` | 0.68 | 0.1–5.0 | °C of supply boost per °C below the threshold |
| `chofu/cmd/stooklijn_uit` | `STOOKLIJN_UIT_GRENS` | 15°C | 5–30°C | Outdoor temp above which heating is switched off |
| `chofu/cmd/water_setpoint` | `t_water_gewenst` | 32°C | 25–55°C | Fixed supply setpoint for `WATER` mode only |

Current values are published (without retain) on:
`chofu/doel_setpoint`, `chofu/stooklijn_grens`, `chofu/stooklijn_factor`, `chofu/stooklijn_uit`, `chofu/water_setpoint`

The same parameters are also editable in the built-in **web UI** (port 80 on the Arduino's IP address).

### Home Assistant

The heating curve parameters appear automatically in Home Assistant as number entities after MQTT auto-discovery (no configuration needed). Look for entities named **Chofu Stooklijn Grens**, **Chofu Stooklijn Factor**, and **Chofu Stooklijn Uit** in the Chofu device.

---

## Tuning Guide

**House is too cold at low outdoor temperatures:**
→ Increase `STOOKLIJN_FACTOR` (e.g. 0.68 → 0.85). The curve gets steeper.

**House overshoots on mild days (e.g. 8–12°C outside):**
→ Lower `STOOKLIJN_GRENS` (e.g. 15 → 10°C) so the boost only activates in harder frost.

**Heat pump runs unnecessarily in spring/autumn:**
→ Lower `STOOKLIJN_UIT_GRENS` (e.g. 15 → 12°C).

**Flat part of the curve is too high or too low:**
→ Adjust `setpoint` (base supply temperature).

**Using `FF_AUTO` and room temperature is consistently off:**
→ Adjust `ff_UA_house` via `chofu/cmd/ff_ua_house` rather than the heating curve — the feedforward model is more direct and self-learns anyway.
