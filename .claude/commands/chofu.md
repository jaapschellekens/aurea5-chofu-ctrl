# Chofu Warmtepomp Controller — Projectkennis

## Taalvoorkeur

Alle documentatie (docs/) wordt in het **Nederlands** geschreven.

Je werkt aan een Arduino UNO R4 WiFi (of ESP32) controller voor een Chofu/Atlantic Aurea 5 hybride warmtepomp met Home Assistant MQTT integratie.

## Firmwarebestand

De actieve firmware is **`chofu_wp_ff/chofu_wp_ff.ino`**. Er is ook een oudere niet-FF versie (`chofu_wp/chofu_wp.ino`) die dezelfde config.h aanpak gebruikt maar zonder feedforward controller.

## Architectuur

### Regelingsmodi (`enum class Modus`)
```
AUTO       — Kamer PID: stooklijn berekent doel-aanvoertemp, PID regelt stand
FF_AUTO    — Feedforward op kamertemperatuur: stand_ff = P_nodig / COP
WATER      — Aanvoer PID op vast water setpoint (t_water_gewenst)
FF_WATER   — Feedforward op aanvoertemperatuur, leert UA_emitter online
HANDMATIG  — Vaste stand 0–12 via MQTT
```

### ControllerState struct (`ctrl`)
Alle regelaarstoestand zit in één struct met reset-methodes:
- `ctrl.stand` — compressorstand 0–8 (auto) of 0–12 (handmatig)
- `ctrl.wp_aan` — warmtepomp actief
- `ctrl.pid_integraal`, `ctrl.pid_vorige_fout`, `ctrl.pid_output`
- `ctrl.ff_integraal` — integraalterm FF-regelaar
- `ctrl.vorige_stand_wijz_ms` — hysteresis timer

Reset-methodes (altijd gebruiken, nooit losse assignments):
- `ctrl.zet_uit()` — stand=0, wp_aan=false, alle integralen=0
- `ctrl.reset_pid()` — alleen PID integralen
- `ctrl.reset_ff()` — alleen FF integraal

### Feedforward regelaar
```
P_nodig  = ff_UA_house × (t_kamer_gewenst − t_outside)
COP      = 0.40 × (T_aanvoer_K / (T_aanvoer_K − T_buiten_K)), geclampt 1–6
stand_ff = laagste stand waarbij VERMOGEN[s] ≥ P_nodig / COP
```
Integraalcorrectie (±2 stand) compenseert modelfout. Boven setpoint: int_corr geclampt op 0 (eerst terugregelen, niet boven equilibrium houden).

### Online leren UA-waarden
- `ff_UA_house` — geleerd uit `P_hp / (T_kamer − T_buiten)`
- `ff_UA_emitter` — geleerd uit `P_hp / (T_aanvoer − T_kamer)`
- Leert **alleen bij thermisch evenwicht**: `|regel_fout| < 0.5°C` (auto) of `< 2.0°C` (water)
- `FF_LEARN_RATE = 0.0002` → tijdconstante ~7 uur

### Auto PID anti-windup
```cpp
float pid_output_raw = Kp*fout + Ki*integraal + Kd*diff + ...;
// Integreer alleen als output niet gesatureerd is in dezelfde richting:
if(!((pid_output_raw > 100 && fout > 0) || (pid_output_raw < 0 && fout < 0))){
  ctrl.pid_integraal += fout * 0.005;
}
```

## Bekende valkuilen

### ArduinoMqttClient 256-byte truncatie
Altijd `pl.length()` meegeven aan `beginMessage()`, anders silent truncatie op 256 bytes.

### HA unit validatie
Gebruik `"°C"` (graadbool UTF-8) niet `"C"`. HA valideert unit vs device_class en slaat de entity stil over bij mismatch. Let op `pl.length()` voor de extra byte van het graadbool.

### EEPROM magic byte
`EEPROM_MAGIC = 0xAD`. Bij toevoeging van nieuwe float-velden: verhoog de magic byte **en** voeg `isnan()`-check toe na `EEPROM.get()`. Zonder isnan-check passeert NaN de range-check `< 50 || > 500` silent.

### SoftwareSerial crasht WiFi
Gebruik uitsluitend `Serial1` (hardware UART, pins D0/D1) voor de WP-communicatie. SoftwareSerial conflicteert met de WiFi co-processor van de UNO R4.

### MQTT retain
- Alle `homeassistant/*/config` discovery berichten: `retain=true`
- `chofu/status = "online"`: `retain=true`
- LWT: `beginWill("chofu/status", true, 1)` → "offline"
- Simulator `chofu/sim/*` en `anna/temperatuur`: `retain=True` (anders verliest Arduino de waarden na MQTT reconnect)

### ESP32 EEPROM
Op ESP32: `EEPROM.begin(64)` in setup, `EEPROM.commit()` na elke schrijfactie. De macros `EEPROM_BEGIN()` en `EEPROM_COMMIT()` in de code zijn no-ops op UNO R4 en actief op ESP32.

## Board-compatibiliteit

De firmware compileert op **beide** boards via `#if defined(ARDUINO_UNOR4_WIFI)` guards:
- WiFi: `WiFiS3.h` (UNO R4) vs `WiFi.h` (ESP32)
- LED Matrix: alleen op UNO R4
- Serial1: `begin(9600)` (UNO R4) vs `begin(9600, SERIAL_8N1, CHOFU_RX_PIN, CHOFU_TX_PIN)` (ESP32, standaard GPIO 16/17)

## Credentials

Staan in `chofu_wp_ff/config.h` (niet in git, staat in `.gitignore`).
Template: `chofu_wp_ff/config.h.example`.

## Simulator (python/wp_simulator.py)

Hardware-in-the-loop test zonder echte WP. Simuleert huismodel (UA_house=263 W/K, C_th=12.5 MJ/K) via MQTT.

```bash
python python/wp_simulator.py --host 192.168.1.x --modus ff_auto --outside 3.0 --speed 6
```

Arduino leest `chofu/sim/supply`, `chofu/sim/outside` etc. en stuurt `chofu/stand` terug.
Belangrijk: `--modus` publiceert met `retain=True`, anders wint een retained oud commando van de broker.

## Geoptimaliseerde parameters (KGE-gecalibreerd)

```
PID:  Kp=19.9  Ki=0.084  Kd=0.036
FF:   UA_house=272.5 W/K  UA_emitter=267.5 W/K
      FF_KI_AUTO=0.026    FF_COAST_AUTO=0.54°C
      FF_KI_WATER=0.017   FF_COAST_WATER=4.76°C
Stooklijn: grens=15°C  factor=0.68
```

## MQTT topics (selectie)

| Topic | Richting | Omschrijving |
|---|---|---|
| `chofu/cmd/modus` | HA→Arduino | `auto` / `ff_auto` / `water` / `ff_water` / `handmatig` |
| `chofu/cmd/stand` | HA→Arduino | 0–12 (zet modus op handmatig) |
| `chofu/cmd/ff_ua_house` | HA→Arduino | UA_house overschrijven (W/K) |
| `chofu/cmd/ff_save` | HA→Arduino | Leerwaarden opslaan in EEPROM |
| `chofu/ff_ua_house` | Arduino→HA | Geleerde UA_house |
| `anna/setpoint` | extern→Arduino | Kamer setpoint (Anna thermostaat) |
| `anna/temperatuur` | extern→Arduino | Kamertemperatuur |
