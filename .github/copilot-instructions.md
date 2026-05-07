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
