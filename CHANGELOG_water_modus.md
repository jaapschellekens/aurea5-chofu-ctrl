# Wijzigingen: Water Modus & Handmatige Stand

**Datum:** 2026-04-28  
**Bestand:** `kromhout_wp_v1_0_CLEAN.ino`

---

## Aanleiding

Uitbreiding van de controller met twee nieuwe features:
1. **Water modus** — stuur een gewenste aanvoertemperatuur via MQTT, de PID regelt de stand om daar te komen
2. **Handmatige stand** — stel compressorstand 0-7 direct in via MQTT (was alleen aan/uit)

---

## Nieuwe variabele

```cpp
float t_water_gewenst = 40.0;  // Gewenste aanvoertemperatuur, instelbaar via MQTT
```

Niet opgeslagen in EEPROM — reset naar 40°C na herstart.

---

## Nieuwe MQTT commando's

| Topic | Payload | Effect |
|-------|---------|--------|
| `chofu/cmd/water_setpoint` | `25.0` – `55.0` (float °C) | Zet gewenste aanvoertemperatuur |
| `chofu/cmd/stand` | `0` – `7` (integer) | Zet vaste stand, schakelt naar handmatig |
| `chofu/cmd/modus` | `"water"` | Schakel naar water modus |

Nieuw state topic:
| Topic | Beschrijving |
|-------|-------------|
| `chofu/water_setpoint` | Huidige water setpoint (gepubliceerd elke 10s) |

---

## Water modus logica

Activeren: `chofu/cmd/modus = "water"`

Regelgedrag (voorbeeld setpoint 40°C):
```
41.0°C ════════ UIT trigger (setpoint + 1°C) ✋
40.5°C          Tolerantieband - huidige staat handhaven
40.0°C ──────── DOEL ✅
39.5°C          Tolerantieband - huidige staat handhaven
39.0°C ════════ AAN trigger (setpoint - 1°C) 🔥
```

- Geen kamertemperatuur correctie — puur op aanvoertemperatuur
- Zelfde PID parameters (Kp/Ki/Kd) als auto modus
- Grote fout (>5°C): versnelde hysteresis (2 min i.p.v. 10 min)
- Vorstbeveiliging blijft altijd actief

---

## Handmatige stand

`chofu/cmd/stand = "4"` → schakelt naar handmatig modus, zet stand op 4 (850W)

Verschil met bestaand `chofu/cmd/power`:
- `cmd/power` → alleen aan (stand 1) of uit (stand 0)
- `cmd/stand` → specifieke stand 0-7 instelbaar

---

## Home Assistant

Twee nieuwe entities via MQTT Auto-Discovery:
- **`number` Chofu Water Setpoint** — instelbaar 25–55°C, stap 0.5°C
- **`number` Chofu Handmatig Stand** — instelbaar 0–7, stap 1

Modus select bijgewerkt naar drie opties: `auto`, `handmatig`, `water`

---

## Gewijzigde bestanden

- `kromhout_wp_v1_0_CLEAN.ino` — controller logica
- `MQTT_REFERENCE.md` — nieuwe secties Water Modus en Handmatige Stand
- `README.md` — features lijst en MQTT topics overzicht bijgewerkt

---

## Testen (Wokwi + publieke MQTT broker)

```bash
# Schakel naar water modus en stel setpoint in
mosquitto_pub -h test.mosquitto.org -t chofu/cmd/modus -m "water"
mosquitto_pub -h test.mosquitto.org -t chofu/cmd/water_setpoint -m "35"

# Verwacht: t_supply=25 → fout=10°C → stand 7, PID ~100%
mosquitto_sub -h test.mosquitto.org -t "chofu/#" -v

# Test UIT schakelen (setpoint onder t_supply - 1)
mosquitto_pub -h test.mosquitto.org -t chofu/cmd/water_setpoint -m "23"
# Verwacht: stand=0, aan=0

# Handmatige stand
mosquitto_pub -h test.mosquitto.org -t chofu/cmd/stand -m "4"
# Verwacht: modus=handmatig, stand=4
```
