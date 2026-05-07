# Copilot Instructions — Aurea 5 / Chofu Warmtepomp Controller

## What This Project Is

Arduino firmware (UNO R4 WiFi or ESP32) that controls a Chofu/Atlantic Aurea 5 heat pump via a proprietary serial protocol (0x19/0x91 telegrams), with Home Assistant integration over MQTT. The project also includes a Python hardware-in-the-loop simulator.

## Active Firmware

**`chofu_wp_ff/chofu_wp_ff.ino`** is the primary file. The older `chofu_wp/chofu_wp.ino` uses the same structure but has no feedforward controller. Work in the `_ff` variant unless explicitly asked otherwise.

## Building & Flashing

Use **Arduino IDE**. No CLI build system exists.

1. Copy `chofu_wp_ff/config.h.example` → `chofu_wp_ff/config.h` and fill in WiFi/MQTT credentials (never committed).
2. Required libraries (install via Library Manager): `ArduinoMqttClient`, `LiquidCrystal_I2C`, `Arduino_LED_Matrix` (UNO R4 only).
3. Board selection: **Arduino UNO R4 WiFi** or any ESP32 board.

## Running the Simulator

```bash
python python/wp_simulator.py --host <MQTT_BROKER_IP> --modus ff_auto --outside 3.0 --speed 6
```

The simulator publishes `retain=True` on all `chofu/sim/*` and `anna/temperatuur` topics — without retain the Arduino loses values after MQTT reconnect.

## Architecture

### Control Modes (`enum class Modus`)

| Mode | Description |
|---|---|
| `AUTO` | Room PID: heating curve → supply setpoint → PID adjusts compressor stage |
| `FF_AUTO` | Feedforward on room temp: `stand = P_needed / COP`, learns `UA_house` online |
| `WATER` | Supply PID on fixed `t_water_gewenst` |
| `FF_WATER` | Feedforward on supply temp, learns `UA_emitter` online |
| `HANDMATIG` | Fixed stage 0–12 via MQTT |

### Controller State (`ControllerState ctrl`)

All mutable controller state lives in one struct. **Always use the reset methods — never assign fields directly:**

- `ctrl.zet_uit()` — stage=0, wp off, all integrals zeroed
- `ctrl.reset_pid()` — PID integrals only
- `ctrl.reset_ff()` — FF integral only

### Feedforward Controller

```
P_needed = ff_UA_house × (t_room_setpoint − t_outside)
COP      = 0.40 × (T_supply_K / (T_supply_K − T_outside_K)), clamped 1–6
stage_ff = lowest stage where VERMOGEN[s] ≥ P_needed / COP
```

Online learning updates `ff_UA_house` and `ff_UA_emitter` only when `|control_error| < 0.5°C` (auto) or `< 2.0°C` (water). `FF_LEARN_RATE = 0.0002` → ~7h time constant.

### Serial Protocol

25-byte telegrams over `Serial1` (hardware UART, pins D0/D1). **Never use SoftwareSerial** — it crashes the UNO R4 WiFi co-processor. Outgoing: `0x19` command. Incoming: `0x91` status. Checksum = sum of bytes 0–22, mod 256.

## Key Pitfalls

### MQTT message length must be explicit
Always pass `pl.length()` to `beginMessage()`. `ArduinoMqttClient` silently truncates at 256 bytes otherwise.

### Home Assistant unit strings
Use `"°C"` (UTF-8 degree symbol), not `"C"`. HA silently drops entities with mismatched `unit_of_measurement` vs `device_class`. Account for the extra UTF-8 byte in `pl.length()`.

### EEPROM — adding new fields
`EEPROM_MAGIC = 0xAD`. When adding a new persisted float:
1. Increment `EEPROM_MAGIC`
2. Add an `isnan()` guard after `EEPROM.get()` — the range check alone (`< 50 || > 500`) does not catch NaN.

### MQTT retain rules
- All `homeassistant/*/config` discovery messages: `retain=true`
- `chofu/status = "online"`: `retain=true`
- LWT: `beginWill("chofu/status", true, 1)` → `"offline"`

### Board differences (handled via `#if defined(ARDUINO_UNOR4_WIFI)`)
- WiFi: `WiFiS3.h` (UNO R4) vs `WiFi.h` (ESP32)
- LED Matrix: UNO R4 only
- EEPROM: UNO R4 writes directly; ESP32 requires `EEPROM.begin(64)` + `EEPROM.commit()` — use the `EEPROM_BEGIN()` / `EEPROM_COMMIT()` macros
- Serial1 init: UNO R4 `begin(9600)` vs ESP32 `begin(9600, SERIAL_8N1, CHOFU_RX_PIN, CHOFU_TX_PIN)`

## Heating Curve (Stooklijn)

### What it does

The heating curve raises the supply water temperature setpoint as outdoor temperature drops. Its formula is:

```
doel_setpoint = min(45°C,  setpoint + (STOOKLIJN_GRENS − t_outside) × STOOKLIJN_FACTOR)
                                       only when t_outside < STOOKLIJN_GRENS
```

With defaults (`setpoint=28°C`, `STOOKLIJN_GRENS=15°C`, `STOOKLIJN_FACTOR=0.68`):

| t_outside | doel_setpoint |
|-----------|--------------|
| 15°C      | 28.0°C (flat, no boost) |
| 10°C      | 31.4°C |
|  5°C      | 34.8°C |
|  0°C      | 38.2°C |
| −5°C      | 41.6°C |
| −7°C      | 43.2°C |
| −10°C     | 45.0°C (capped) |

### Role per control mode

| Mode | How the heating curve is used |
|------|-------------------------------|
| `AUTO` | `doel_setpoint` is the PID error reference: `aanvoer_fout = doel_setpoint − t_supply`. The PID drives the compressor stage to reach this supply temperature. |
| `FF_AUTO` | `doel_setpoint` is computed the same way but the FF controller uses `UA_house × (t_room_setpoint − t_outside) / COP` directly — the heating curve result is published to `chofu/doel_setpoint` for monitoring but does **not** enter the FF math. |
| `WATER` | Heating curve is **not used**. The PID tracks `t_water_gewenst` (fixed water setpoint, default 32°C, adjustable via `chofu/cmd/water_setpoint`). |
| `FF_WATER` | The heating curve **replaces** `t_water_gewenst` as the FF supply setpoint: `wsp = stooklijn`. The FF then targets `wsp` instead of a fixed temperature, giving automatic weather compensation in this mode. |
| `HANDMATIG` | Not used. |

### Shut-off threshold (`STOOKLIJN_UIT_GRENS`)

In `AUTO` and `FF_AUTO`, when `t_outside > STOOKLIJN_UIT_GRENS` (default 15°C) the heat pump is switched off entirely — heating season is over. This threshold is independent of `STOOKLIJN_GRENS`.

### Frost protection override

In `AUTO` and `FF_AUTO`, when `t_outside < T_VORST` (default 4°C) and the heat pump is off, it is forced to stage 1 regardless of the room temperature. This overrides the normal shut-off logic.

### Adjusting the heating curve

All parameters are persisted in EEPROM and adjustable via MQTT (no reflash needed):

| MQTT command topic | Variable | Default | Range | Effect |
|--------------------|----------|---------|-------|--------|
| `chofu/cmd/stooklijn_grens` | `STOOKLIJN_GRENS` | 15°C | 0–25°C | Outdoor temp at which boost starts. Lower = boost only in harder frost. |
| `chofu/cmd/stooklijn_factor` | `STOOKLIJN_FACTOR` | 0.68 | 0.1–5.0 | °C of supply boost per °C below the grens. Higher = steeper curve. |
| `chofu/cmd/stooklijn_uit` | `STOOKLIJN_UIT_GRENS` | 15°C | 5–30°C | Outdoor temp above which heating stops. |
| `chofu/cmd/water_setpoint` | `t_water_gewenst` | 32°C | 25–55°C | Fixed supply setpoint for `WATER` mode only. |
| `chofu/cmd/setpoint` | `setpoint` | 28°C | — | Base supply setpoint (the flat part of the curve). |

All changes are saved to EEPROM immediately. Current values are published on `chofu/stooklijn_grens`, `chofu/stooklijn_factor`, `chofu/stooklijn_uit`, `chofu/water_setpoint`, and `chofu/doel_setpoint`.

The same parameters are also editable in the built-in web UI served on port 80.

### Tuning guidance

- If the house is **too cold at low outdoor temps**: increase `STOOKLIJN_FACTOR` (e.g. 0.68 → 0.85).
- If the house **overshoots on mild days**: lower `STOOKLIJN_GRENS` (e.g. 15 → 10°C) so the boost only kicks in when it's actually cold.
- If the heat pump **runs unnecessarily in spring/autumn**: lower `STOOKLIJN_UIT_GRENS`.
- `FF_AUTO` is largely self-tuning via `UA_house` learning — prefer adjusting `ff_UA_house` (via `chofu/cmd/ff_ua_house`) over the heating curve parameters when in FF modes.

## Calibrated Parameters

Do not change these without re-running the KGE calibration:

```
PID:  Kp=19.9  Ki=0.084  Kd=0.036
FF:   UA_house=272.5 W/K   UA_emitter=267.5 W/K
      FF_KI_AUTO=0.026      FF_COAST_AUTO=0.54°C
      FF_KI_WATER=0.017     FF_COAST_WATER=4.76°C
Heating curve: grens=15°C  factor=0.68
```

## Key MQTT Topics

| Topic | Direction | Description |
|---|---|---|
| `chofu/cmd/modus` | HA→Arduino | `auto` / `ff_auto` / `water` / `ff_water` / `handmatig` |
| `chofu/cmd/stand` | HA→Arduino | Stage 0–12 (sets mode to `handmatig`) |
| `chofu/cmd/ff_ua_house` | HA→Arduino | Override UA_house (W/K) |
| `chofu/cmd/ff_save` | HA→Arduino | Save learned values to EEPROM |
| `chofu/ff_ua_house` | Arduino→HA | Current learned UA_house |
| `anna/setpoint` | extern→Arduino | Room setpoint (Anna thermostat) |
| `anna/temperatuur` | extern→Arduino | Room temperature |
| `chofu/sim/*` | Simulator→Arduino | Hardware-in-the-loop simulation values |
