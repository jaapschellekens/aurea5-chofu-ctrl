# MQTT Referentie - Kromhout Warmtepomp Controller

Complete overzicht van alle MQTT topics, commando's en HA discovery entiteiten.

MQTT Broker: `<YOUR_MQTT_BROKER_IP>:1883`  
Alle topics gebruiken het prefix `chofu/`

---

## State Topics (Arduino → Home Assistant)

Alle state topics worden elke **10 seconden** gepubliceerd. HA discovery zorgt automatisch voor de entiteiten.

### Temperaturen

| Topic | Type | Unit | Beschrijving |
|-------|------|------|--------------|
| `chofu/supply` | float | °C | Aanvoer temperatuur |
| `chofu/return` | float | °C | Retour temperatuur |
| `chofu/outside` | float | °C | Buiten temperatuur |
| `chofu/delta_t` | float | °C | Delta T (aanvoer − retour) |
| `chofu/kamer` | float | °C | Kamer temperatuur (van Anna) |
| `chofu/kamer_gewenst` | float | °C | Gewenste kamer temperatuur (van Anna) |
| `chofu/setpoint` | float | °C | Aanvoer setpoint (auto modus stooklijn) |
| `chofu/water_setpoint` | float | °C | Gewenste aanvoertemperatuur (water modus) |
| `chofu/t_vorst` | float | °C | Actieve vorstgrens |

### Status

| Topic | Type | Values | Beschrijving |
|-------|------|--------|--------------|
| `chofu/stand` | int | 0–12 | Huidige compressor stand |
| `chofu/vermogen` | int | W | Geschat vermogen |
| `chofu/aan` | string | 0/1 | Warmtepomp aan/uit |
| `chofu/modus` | string | auto/ff_auto/water/ff_water/handmatig | Regelingsmodus |
| `chofu/doel_setpoint` | float | °C | Actief doel-setpoint (stooklijn) |
| `chofu/koeling` | string | 0/1 | Koelstand actief |
| `chofu/pid` | float | % | PID/FF output |
| `chofu/ff_ua_house` | float | W/K | Geleerde UA huis (FF auto) |
| `chofu/ff_ua_emitter` | float | W/K | Geleerde UA emitter (FF water) |
| `chofu/defrost` | string | 0/1 | Defrost actief |
| `chofu/pomp` | int | 0–100 | Pompsnelheid (%) |
| `chofu/comp_hz` | int | Hz | Compressor frequentie |
| `chofu/lcd` | string | 0/1 | LCD backlight status |
| `chofu/sim_actief` | string | 0/1 | Simulatiemodus actief |

### Safeguard Grenzen (instelbaar)

| Topic | Type | Unit | Default | Beschrijving |
|-------|------|------|---------|--------------|
| `chofu/supply_max` | float | °C | 60.0 | Noodstop aanvoer temperatuur |
| `chofu/koeling_min_buiten` | float | °C | 18.0 | Minimale buitentemp voor koeling |
| `chofu/supply_min` | float | °C | 17.0 | Condensatiebescherming koeling |
| `chofu/koeling_afschakel` | float | °C | 0.5 | Afschakeldrempel koeling (niet EEPROM) |
| `chofu/stooklijn_uit` | float | °C | 15.0 | Boven deze buitentemp: verwarming uit (auto) |

### Systeem

| Topic | Type | Retained | Beschrijving |
|-------|------|----------|--------------|
| `chofu/status` | string | ✅ | `online` / `offline` (LWT) |
| `chofu/alert` | string | ✅ | Laatste waarschuwing of foutmelding |
| `chofu/log/INFO` | string | ❌ | Normale events |
| `chofu/log/WARNING` | string | ❌ | Waarschuwingen (ook via alert) |
| `chofu/log/ERROR` | string | ❌ | Fouten |

> `chofu/alert` is retained: HA toont altijd het laatste bericht, ook na herstart.

---

## Command Topics (Home Assistant → Arduino)

### Modus

```
Topic:   chofu/cmd/modus
Payload: "auto"      → PID regeling op kamertemperatuur (Anna)
         "ff_auto"   → Feedforward op kamertemperatuur (leert UA_house)
         "water"     → PID regeling op aanvoertemperatuur
         "ff_water"  → Feedforward op aanvoertemperatuur (leert UA_emitter)
         "handmatig" → Vaste stand via chofu/cmd/stand
```

### Power

```
Topic:   chofu/cmd/power
Payload: "1" → AAN (schakelt naar handmatig stand 1)
         "0" → UIT (handmatig stand 0)
```

### Handmatige Stand

```
Topic:   chofu/cmd/stand
Payload: "0" – "12"  (integer)

Schakelt automatisch naar handmatige modus.
Stands 9–12 zijn alleen beschikbaar in handmatige modus.

Stand mapping:
  0  = UIT           (   0 W)
  1  = Minimum       ( 240 W)
  2  = Laag          ( 420 W)
  3  = Medium-       ( 640 W)
  4  = Medium        ( 850 W)
  5  = Medium+       (1050 W)
  6  = Hoog          (1250 W)
  7  = Hoog+         (1450 W)
  8  = Max auto      (1550 W)  ← max in auto/water modus
  9  = Uitgebreid-   (1650 W)  ← alleen handmatig
 10  = Uitgebreid    (1700 W)  ← alleen handmatig
 11  = Uitgebreid+   (1750 W)  ← alleen handmatig
 12  = Maximum       (1800 W)  ← alleen handmatig
```

### Water Modus

```
Topic:   chofu/cmd/water_setpoint
Payload: "25.0" – "55.0"  (float, °C)

Stel gewenste aanvoertemperatuur in (water modus).
Tolerantie ±1°C:
  setpoint + 1°C → UIT
  setpoint − 1°C → AAN

Niet opgeslagen in EEPROM (reset naar 40°C na herstart).
```

**Regelgedrag water modus (voorbeeld setpoint 40°C):**
```
41.0°C ════ UIT trigger ✋
40.0°C ──── DOEL ✅
39.0°C ════ AAN trigger 🔥

Hysteresis bij standwijziging:
  Omlaag:        5 min  (HYST_DOWN_MS)
  Omhoog groot:  2 min  (HYST_FAST_MS, fout > 5°C)
  Omhoog normaal:10 min (HYST_SLOW_MS)
```

### Koeling

```
Topic:   chofu/cmd/koeling
Payload: "1" → Koelen
         "0" → Verwarmen (default)

Koeling is alleen beschikbaar in ff_auto, ff_water en handmatig.
In auto of water wordt het commando genegeerd met een alert.

FF_AUTO:  P_nodig = UA_house × (t_outside − t_kamer_gewenst)
FF_WATER: P_nodig = UA_emitter × (t_kamer − t_water_gewenst)
HANDMATIG: vaste stand, ongewijzigd.

Zet protocol byte 19-2,3 op 2 (koeling) i.p.v. 1 (verwarming).
Wordt automatisch uitgeschakeld als buiten < KOELING_MIN_BUITEN.
Niet opgeslagen in EEPROM.

Typisch gebruik FF_AUTO:
  chofu/cmd/modus          = "ff_auto"
  chofu/cmd/koeling        = "1"

Typisch gebruik FF_WATER:
  chofu/cmd/modus          = "ff_water"
  chofu/cmd/water_setpoint = "18.0"
  chofu/cmd/koeling        = "1"
```

**Condensatiebescherming (SUPPLY_MIN):**
```
Topic:   chofu/cmd/supply_min
Payload: "10.0" – "25.0"  (float, °C)
Aanvoer daalt nooit onder deze grens (voorkomt condensatie op vloer/plafond).
Opgeslagen in EEPROM. Default: 17.0°C
```

**Afschakeldrempel (KOELING_AFSCHAKEL):**
```
Topic:   chofu/cmd/koeling_afschakel
Payload: "0.1" – "5.0"  (float, °C)
Koeling schakelt terug als kamer/aanvoer meer dan deze waarde onder setpoint daalt.
Niet opgeslagen in EEPROM. Default: 0.5°C
```

### Stooklijn Parameters

```
Topic:   chofu/cmd/setpoint
Payload: "20.0" – "45.0"  (float, °C)
Opgeslagen in EEPROM. Default: 28.0°C

Topic:   chofu/cmd/t_vorst
Payload: "-10.0" – "10.0"  (float, °C)
Vorstgrens: WP blijft minimaal stand 1 bij buiten < T_VORST.
Opgeslagen in EEPROM. Default: 4.0°C

Topic:   chofu/cmd/stooklijn_grens
Payload: "0.0" – "25.0"  (float, °C)
Onder deze buitentemp wordt het setpoint verhoogd. Default: 15.0°C

Topic:   chofu/cmd/stooklijn_factor
Payload: "0.1" – "5.0"  (float)
°C setpointverhoging per °C onder de grens. Default: 0.68
Voorbeeld: buiten 5°C, grens 15°C, factor 0.68 → +6.8°C setpoint
```

### PID Parameters

```
Topic:   chofu/cmd/kp     Payload: float  Default: 19.9
Topic:   chofu/cmd/ki     Payload: float  Default: 0.084
Topic:   chofu/cmd/kd     Payload: float  Default: 0.036

Alle PID parameters opgeslagen in EEPROM.
```

### Feedforward Parameters

```
Topic:   chofu/cmd/ff_ua_house
Payload: "50" – "500"  (float, W/K)
UA_house overschrijven. Wordt ook online bijgeleerd.
Opgeslagen in EEPROM. Default: 272.5 W/K

Topic:   chofu/cmd/ff_ua_emitter
Payload: "50" – "500"  (float, W/K)
UA_emitter overschrijven. Wordt ook online bijgeleerd.
Opgeslagen in EEPROM. Default: 267.5 W/K

Topic:   chofu/cmd/ff_save
Payload: "1"
Sla de huidige geleerde UA-waarden op in EEPROM.
```

### Safeguard Grenzen

```
Topic:   chofu/cmd/supply_max
Payload: "40.0" – "80.0"  (float, °C)
Noodstop: aanvoer boven deze temp → stand 0, ongeacht modus.
Opgeslagen in EEPROM. Default: 60.0°C

Topic:   chofu/cmd/koeling_min_buiten
Payload: "0.0" – "30.0"  (float, °C)
Koeling geblokkeerd als buiten < deze grens.
Opgeslagen in EEPROM. Default: 18.0°C

Topic:   chofu/cmd/stooklijn_uit
Payload: "5.0" – "30.0"  (float, °C)
Auto modus: verwarming stopt als buiten > deze grens.
Opgeslagen in EEPROM. Default: 15.0°C
```

### Overig

```
Topic:   chofu/cmd/lcd
Payload: "1" → LCD backlight aan
         "0" → LCD backlight uit

Topic:   chofu/cmd/force_start
Payload: "1"
Reset hysteresis timer → warmtepomp start direct.
```

---

## Safeguards

Overzicht van alle ingebouwde beveiligingen:

| Safeguard | Conditie | Actie | Instelbaar |
|-----------|----------|-------|------------|
| **Noodstop aanvoer** | `t_supply > SUPPLY_MAX` (60°C) | Stand 0, WP uit | `chofu/cmd/supply_max` |
| **Vorstbeveiliging** | `t_outside < T_VORST` (4°C) | Minimaal stand 1 | `chofu/cmd/t_vorst` |
| **Koeling blokkering** | `koeling=1` en `buiten < 18°C` | Koeling automatisch uit | `chofu/cmd/koeling_min_buiten` |
| **Stooklijn uit** | `buiten > 15°C` in auto modus | Stand 0, WP uit | `chofu/cmd/stooklijn_uit` |
| **Spike filter aanvoer/retour** | Sprong > 10°C per cyclus | Waarde verworpen, vorige behouden | — |
| **Spike filter buiten** | Sprong > 5°C of buiten −30..+50°C | Waarde verworpen | — |
| **MQTT watchdog** | Geen MQTT > 120 min | Water/handmatig → auto | — |

Alle safeguard meldingen verschijnen op `chofu/alert` (retained) en `chofu/log/WARNING`.

### Alert Voorbeelden

```
"NOODSTOP aanvoer: 63.2C > max 60.0C"
"Koeling geblokkeerd: buiten 15.3C < min 18.0C"
"Spike aanvoer: 72.1C verworpen (was 42.3C)"
"Spike buiten: 88.5C verworpen"
"Verwarming gestopt: buiten 16.2C > 15.0C"
"MQTT watchdog: geen contact > 120 min, terug naar auto"
"❄️ VORSTBEVEILIGING! Buiten: 3.2°C → Stand 1"
```

---

## Home Assistant Auto-Discovery

De Arduino publiceert alle HA discovery configs automatisch bij opstart (retained). Handmatige YAML configuratie is **niet** nodig.

**Entiteiten (26 totaal):**

| Entiteit | Type | Topic |
|----------|------|-------|
| Chofu Aanvoer | sensor (°C) | `chofu/supply` |
| Chofu Retour | sensor (°C) | `chofu/return` |
| Chofu Buiten | sensor (°C) | `chofu/outside` |
| Chofu Delta T | sensor (°C) | `chofu/delta_t` |
| Chofu Kamer | sensor (°C) | `chofu/kamer` |
| Chofu Kamer Gewenst | sensor (°C) | `chofu/kamer_gewenst` |
| Chofu Setpoint | sensor (°C) | `chofu/setpoint` |
| Chofu Doel Setpoint | sensor (°C) | `chofu/doel_setpoint` |
| Chofu Stand | sensor | `chofu/stand` |
| Chofu Vermogen | sensor (W) | `chofu/vermogen` |
| Chofu Modus | sensor | `chofu/modus` |
| Chofu PID | sensor (%) | `chofu/pid` |
| Chofu Pomp | sensor | `chofu/pomp` |
| Chofu Comp Hz | sensor (Hz) | `chofu/comp_hz` |
| Chofu Defrost | binary_sensor | `chofu/defrost` |
| Chofu Simulatie | binary_sensor | `chofu/sim_actief` |
| Chofu Alert | sensor | `chofu/alert` |
| Chofu Power | switch | `chofu/cmd/power` |
| Chofu LCD | switch | `chofu/cmd/lcd` |
| Chofu Koeling | switch | `chofu/cmd/koeling` |
| Chofu Koeling Aanvoer Min | number (°C) | `chofu/cmd/supply_min` |
| Chofu Koeling Afschakeldrempel | number (°C) | `chofu/cmd/koeling_afschakel` |
| Chofu Modus Select | select | `chofu/cmd/modus` |
| Chofu Water SP | number (°C) | `chofu/cmd/water_setpoint` |
| Chofu Vorstgrens | number (°C) | `chofu/cmd/t_vorst` |
| Chofu Stand (handmatig) | number | `chofu/cmd/stand` |
| Chofu Aanvoer Max | number (°C) | `chofu/cmd/supply_max` |
| Chofu Koeling Min Buiten | number (°C) | `chofu/cmd/koeling_min_buiten` |
| Chofu Stooklijn Uit | number (°C) | `chofu/cmd/stooklijn_uit` |

**Availability:** Alle entiteiten zijn gekoppeld aan `chofu/status`. Bij verbindingsverlies (LWT) zet de broker automatisch `offline` en markeert HA alle entiteiten als niet beschikbaar.

---

## Simulatie Topics (Testen Zonder Hardware)

Met deze topics kunnen sensorwaarden worden overschreven voor testen zonder hardware aansluiting. De simulatiewaarden worden elke loop-iteratie toegepast vóór de PID berekening en alle safeguards.

**`chofu/sim_actief`** (state, retained) toont in HA of simulatie actief is.

### Sensorwaarden injecteren

```
Topic:   chofu/sim/supply
Payload: float °C  (bereik -10 tot 80)
         leeg of "reset" → terug naar echte sensorwaarde

Topic:   chofu/sim/return
Payload: float °C  (bereik -10 tot 80)
         leeg of "reset" → terug naar echte sensorwaarde

Topic:   chofu/sim/outside
Payload: float °C  (bereik -30 tot 50)
         leeg of "reset" → terug naar echte sensorwaarde

Topic:   chofu/sim/water_setpoint
Payload: float °C  (bereik 25 tot 55)
         leeg of "reset" → terug naar waarde van chofu/cmd/water_setpoint

Topic:   chofu/sim/kamer
Payload: float °C  (bereik 5 tot 40)
         Overschrijft anna/temperatuur in de PID-regeling.
         Gebruik dit in simulatie zodat de echte Zigbee-sensor
         (die ook naar anna/temperatuur publiceert) niet interfereert.
         leeg of "reset" → terug naar anna/temperatuur

Topic:   chofu/sim/kamer_gewenst
Payload: float °C  (bereik 14 tot 30)
         Overschrijft anna/setpoint in de PID-regeling.
         leeg of "reset" → terug naar anna/setpoint

Topic:   chofu/sim/reset
Payload: willekeurig
Effect:  Wist alle simulatiewaarden tegelijk
         (supply, return, outside, water_setpoint, kamer, kamer_gewenst)
```

### Testscenarios

```bash
BROKER="<YOUR_MQTT_BROKER_IP>"

# Noodstop aanvoer (SUPPLY_MAX = 60°C)
mosquitto_pub -h $BROKER -t "chofu/sim/supply" -m "65.0"
# Verwacht: stand=0, alert op chofu/alert

# Spike filter aanvoer (>10°C sprong)
mosquitto_pub -h $BROKER -t "chofu/sim/supply" -m "99.0"
# Verwacht: waarde verworpen, vorige waarde behouden, alert

# Koeling blokkering (buiten < 18°C)
mosquitto_pub -h $BROKER -t "chofu/cmd/koeling" -m "1"
mosquitto_pub -h $BROKER -t "chofu/sim/outside" -m "10.0"
# Verwacht: koeling=0, alert

# Stooklijn uit (auto modus, buiten > 15°C)
mosquitto_pub -h $BROKER -t "chofu/cmd/modus" -m "auto"
mosquitto_pub -h $BROKER -t "chofu/sim/outside" -m "20.0"
# Verwacht: stand=0, alert

# Vorstbeveiliging (buiten < T_VORST = 4°C)
mosquitto_pub -h $BROKER -t "chofu/sim/outside" -m "2.0"
# Verwacht: stand minimaal 1

# Water modus PID testen
mosquitto_pub -h $BROKER -t "chofu/cmd/modus" -m "water"
mosquitto_pub -h $BROKER -t "chofu/sim/water_setpoint" -m "40.0"
mosquitto_pub -h $BROKER -t "chofu/sim/supply" -m "35.0"
# Verwacht: PID verhoogt stand (5°C te koud → snelle hysteresis)
mosquitto_pub -h $BROKER -t "chofu/sim/supply" -m "42.0"
# Verwacht: PID verlaagt stand (te warm, maar binnen 1°C tolerantie)
mosquitto_pub -h $BROKER -t "chofu/sim/supply" -m "41.5"
# Verwacht: WP uit (>1°C boven setpoint)

# Koeling testen met sim setpoint
mosquitto_pub -h $BROKER -t "chofu/cmd/koeling" -m "1"
mosquitto_pub -h $BROKER -t "chofu/sim/water_setpoint" -m "18.0"
mosquitto_pub -h $BROKER -t "chofu/sim/supply" -m "20.0"
# Verwacht: PID verhoogt stand (te warm water bij koeling)

# Buiten sanity check
mosquitto_pub -h $BROKER -t "chofu/sim/outside" -m "99.0"
# Verwacht: waarde verworpen (buiten -30..+50 bereik), alert

# Simulatie resetten
mosquitto_pub -h $BROKER -t "chofu/sim/reset" -m "1"
# Verwacht: alle sim waarden weg, echte sensorwaarden actief
```

### Monitoring tijdens testen

```bash
# Alles tegelijk volgen
mosquitto_sub -h <YOUR_MQTT_BROKER_IP> -t "chofu/#" -v

# Alleen alerts en logs
mosquitto_sub -h <YOUR_MQTT_BROKER_IP> -t "chofu/alert" -v
mosquitto_sub -h <YOUR_MQTT_BROKER_IP> -t "chofu/log/#" -v
```

> **Let op:** Simulatiewaarden overleven geen herstart van de Arduino. Na een reset zijn alle sim waarden NAN (niet ingesteld) en worden echte sensorwaarden gebruikt.

---

## Simulatie Timing (Sneller-dan-realtime replay)

Voor het replay van historische tijdreeksen via een Python script kan de timing van de PID-regeling en hysteresis worden versneld via MQTT. Deze waarden worden **niet** in EEPROM opgeslagen — een herstart herstelt altijd de productiewaarden.

| Topic | Standaard | Eenheid | Beschrijving |
|-------|-----------|---------|--------------|
| `chofu/cmd/hyst_slow` | 600000 | ms | Hysteresis bij standverhoging (normaal: 10 min) |
| `chofu/cmd/hyst_fast` | 120000 | ms | Hysteresis bij grote fout >1°C (normaal: 2 min) |
| `chofu/cmd/hyst_down` | 300000 | ms | Hysteresis bij standverlaging (normaal: 5 min) |
| `chofu/cmd/pid_interval` | 5000 | ms | PID berekeninterval (normaal: 5 sec) |

State topics (gepubliceerd elke 10 sec): `chofu/hyst_slow`, `chofu/hyst_fast`, `chofu/hyst_down`, `chofu/pid_interval`

Validatiebereiken: hyst_* 100–3.600.000 ms, pid_interval 100–60.000 ms.

### Python replay gebruik

```bash
# Versnelling instellen voor simulatie (bijv. 60x sneller: 1 min → 1 sec)
BROKER=<YOUR_MQTT_BROKER_IP>
mosquitto_pub -h $BROKER -t "chofu/cmd/hyst_slow"    -m "10000"   # 10 sec
mosquitto_pub -h $BROKER -t "chofu/cmd/hyst_fast"    -m "2000"    # 2 sec
mosquitto_pub -h $BROKER -t "chofu/cmd/hyst_down"    -m "5000"    # 5 sec
mosquitto_pub -h $BROKER -t "chofu/cmd/pid_interval" -m "83"      # ~1/60 van 5000ms

# Na simulatie: herstellen (of gewoon Arduino herstarten)
mosquitto_pub -h $BROKER -t "chofu/cmd/hyst_slow"    -m "600000"
mosquitto_pub -h $BROKER -t "chofu/cmd/hyst_fast"    -m "120000"
mosquitto_pub -h $BROKER -t "chofu/cmd/hyst_down"    -m "300000"
mosquitto_pub -h $BROKER -t "chofu/cmd/pid_interval" -m "5000"
```

> **Veiligheid:** Alle safeguards (SUPPLY_MAX, KOELING_MIN_BUITEN, vorstbeveiliging) blijven actief met versnelde timing. Alleen de schakelfrequentie neemt toe.

---

## Anna Thermostaat Topics

| Topic | Richting | Beschrijving |
|-------|----------|--------------|
| `anna/setpoint` | Anna → Arduino | Gewenste kamertemperatuur (14–30°C) |
| `anna/temperatuur` | Anna → Arduino | Werkelijke kamertemperatuur (5–35°C) |

> **Simulatie:** Gebruik `chofu/sim/kamer` en `chofu/sim/kamer_gewenst` in plaats van
> de Anna-topics. Zo overschrijft de echte Zigbee-sensor de simulatiewaarden niet.

**Home Assistant Automatisering:**
```yaml
automation:
  - alias: "Anna Setpoint naar MQTT"
    trigger:
      platform: state
      entity_id: climate.anna
    action:
      service: mqtt.publish
      data:
        topic: "anna/setpoint"
        payload: "{{ state_attr('climate.anna', 'temperature') }}"

  - alias: "Anna Temperatuur naar MQTT"
    trigger:
      platform: state
      entity_id: sensor.anna_temperature
    action:
      service: mqtt.publish
      data:
        topic: "anna/temperatuur"
        payload: "{{ states('sensor.anna_temperature') }}"
```

---

## MQTT Explorer Structuur

```
chofu/
├── supply                   35.2
├── return                   30.1
├── outside                   8.5
├── delta_t                   5.1
├── kamer                    20.5
├── kamer_gewenst            21.0
├── setpoint                 28.0
├── doel_setpoint            34.8
├── water_setpoint           32.0
├── t_vorst                   4.0
├── stand                     3
├── vermogen                640
├── aan                       1
├── modus                ff_auto
├── koeling                   0
├── pid                      37.5
├── ff_ua_house             272.5
├── ff_ua_emitter           267.5
├── defrost                   0
├── pomp                     60
├── comp_hz                  45
├── lcd                       1
├── supply_max               60.0
├── koeling_min_buiten       18.0
├── stooklijn_uit            15.0
├── sim_actief                0
├── hyst_slow            600000   ← simulatie timing (niet EEPROM)
├── hyst_fast            120000
├── hyst_down            300000
├── pid_interval           5000
├── status                online   ← retained, LWT
├── alert          Verwarming gestopt: buiten 16.2C   ← retained
├── log/
│   ├── INFO             laatste event
│   └── WARNING          laatste waarschuwing
├── sim/
│   ├── supply              (schrijf alleen, geen state)
│   ├── return              (schrijf alleen, geen state)
│   ├── outside             (schrijf alleen, geen state)
│   ├── water_setpoint      (schrijf alleen, geen state)
│   ├── kamer               (schrijf alleen — overschrijft anna/temperatuur)
│   ├── kamer_gewenst       (schrijf alleen — overschrijft anna/setpoint)
│   └── reset               (schrijf alleen, geen state)
└── cmd/
    ├── modus
    ├── power
    ├── stand
    ├── water_setpoint
    ├── kamer_setpoint
    ├── koeling
    ├── setpoint
    ├── t_vorst
    ├── supply_max
    ├── koeling_min_buiten
    ├── stooklijn_uit
    ├── stooklijn_grens
    ├── stooklijn_factor
    ├── kp / ki / kd
    ├── ff_ua_house         ← FF leerwaarden
    ├── ff_ua_emitter
    ├── ff_save
    ├── lcd
    ├── force_start
    ├── hyst_slow           ← simulatie timing (niet EEPROM)
    ├── hyst_fast
    ├── hyst_down
    └── pid_interval

anna/
├── setpoint                 21.0
└── temperatuur              20.3
```

---

## Testing Met Mosquitto CLI

```bash
BROKER="<YOUR_MQTT_BROKER_IP>"

# Modus wisselen
mosquitto_pub -h $BROKER -t "chofu/cmd/modus" -m "auto"
mosquitto_pub -h $BROKER -t "chofu/cmd/modus" -m "water"
mosquitto_pub -h $BROKER -t "chofu/cmd/modus" -m "handmatig"

# Water modus instellen
mosquitto_pub -h $BROKER -t "chofu/cmd/water_setpoint" -m "45.0"

# Koeling inschakelen (alleen ff_auto, ff_water of handmatig)
mosquitto_pub -h $BROKER -t "chofu/cmd/modus" -m "ff_water"
mosquitto_pub -h $BROKER -t "chofu/cmd/water_setpoint" -m "18.0"
mosquitto_pub -h $BROKER -t "chofu/cmd/koeling" -m "1"

# Handmatige stand
mosquitto_pub -h $BROKER -t "chofu/cmd/stand" -m "4"   # 850W
mosquitto_pub -h $BROKER -t "chofu/cmd/stand" -m "0"   # uit

# Safeguard grenzen aanpassen
mosquitto_pub -h $BROKER -t "chofu/cmd/supply_max" -m "55.0"
mosquitto_pub -h $BROKER -t "chofu/cmd/koeling_min_buiten" -m "20.0"
mosquitto_pub -h $BROKER -t "chofu/cmd/stooklijn_uit" -m "18.0"

# Vorstgrens
mosquitto_pub -h $BROKER -t "chofu/cmd/t_vorst" -m "2.0"

# PID tuning
mosquitto_pub -h $BROKER -t "chofu/cmd/kp" -m "0.8"
mosquitto_pub -h $BROKER -t "chofu/cmd/ki" -m "0.01"
mosquitto_pub -h $BROKER -t "chofu/cmd/kd" -m "0.3"

# Force start (reset hysteresis)
mosquitto_pub -h $BROKER -t "chofu/cmd/force_start" -m "1"

# Anna simuleren
mosquitto_pub -h $BROKER -t "anna/setpoint" -m "21.0"
mosquitto_pub -h $BROKER -t "anna/temperatuur" -m "20.3"

# Monitoring
mosquitto_sub -h $BROKER -t "chofu/#" -v
mosquitto_sub -h $BROKER -t "chofu/log/#" -v
mosquitto_sub -h $BROKER -t "chofu/alert" -v
```

---

## Troubleshooting

### Entiteiten niet zichtbaar in HA

```
1. Wacht 2 minuten na herstart Arduino (discovery in 3 fases)
2. Check MQTT Explorer: zijn chofu/* topics aanwezig?
3. Check homeassistant/sensor/chofu_hp/supply/config: is er JSON?
4. HA → Settings → Devices & Services → MQTT → Reload
5. Verwijder device in HA en wacht op herdetectie
```

### Entiteiten "niet beschikbaar"

```
Check chofu/status in MQTT Explorer:
- "online"  → Arduino verbonden, probleem elders
- "offline" → LWT getriggerd (Arduino weg of crash)
- leeg      → Arduino heeft nog niet gepubliceerd
```

### Alert sensor toont oude melding

```
chofu/alert is retained → bevat altijd laatste bericht.
Wis handmatig via MQTT Explorer (leeg bericht sturen)
of wacht op volgende alert.
```

### Commando's werken niet

```
1. mosquitto_pub -h <YOUR_MQTT_BROKER_IP> -t "chofu/cmd/modus" -m "auto"
2. Check Serial monitor: toont "MQTT: chofu/cmd/modus=auto"
3. Als niks: Arduino niet verbonden met broker
```

---

## EEPROM Opslag

Volgende parameters blijven bewaard na herstart:

| Parameter | Default | Commando |
|-----------|---------|---------|
| Setpoint (stooklijn) | 28.0°C | `chofu/cmd/setpoint` |
| Kp | 75.0 | `chofu/cmd/kp` |
| Ki | 0.800 | `chofu/cmd/ki` |
| Kd | 0.010 | `chofu/cmd/kd` |
| Stooklijn grens | 15.0°C | `chofu/cmd/stooklijn_grens` |
| Stooklijn factor | 0.68 | `chofu/cmd/stooklijn_factor` |
| T_VORST | 4.0°C | `chofu/cmd/t_vorst` |
| SUPPLY_MAX | 60.0°C | `chofu/cmd/supply_max` |
| KOELING_MIN_BUITEN | 18.0°C | `chofu/cmd/koeling_min_buiten` |
| SUPPLY_MIN | 17.0°C | `chofu/cmd/supply_min` |
| STOOKLIJN_UIT | 15.0°C | `chofu/cmd/stooklijn_uit` |
| FF UA_house | 272.5 W/K | `chofu/cmd/ff_ua_house` |
| FF UA_emitter | 267.5 W/K | `chofu/cmd/ff_ua_emitter` |

---

*Zie ook: [README.md](README.md) — Projectoverzicht en installatie*
