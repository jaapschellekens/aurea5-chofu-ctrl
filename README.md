# Chofu Warmtepomp Controller

**Arduino UNO R4 WiFi controller voor Chofu (Atlantic aurea 5) hybride warmtepomp met Home Assistant integratie**

> Fork van [kromhoutmaarten-sys/Aurea-5-hybrid_uno4](https://github.com/kromhoutmaarten-sys/Aurea-5-hybrid_uno4)

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Arduino](https://img.shields.io/badge/Arduino-UNO%20R4%20WiFi-00979D?logo=arduino)](https://www.arduino.cc/)
[![Home Assistant](https://img.shields.io/badge/Home%20Assistant-Compatible-41BDF5?logo=home-assistant)](https://www.home-assistant.io/)

---

## Over Dit Project

Dit is een test versie gebaseerd op de originele kromhout code. Het is zo aangepast dat het bij mij werkt (de hardware connectie werkt nog niet!)

Hoewel ik uiteindelijk veel handmatig heb gedaan is dit voor een groot deel met behulp van claude code en copilot gemaakt. 

De controller spreekt het Chofu 0x19/0x91 serieel protocol, regelt het compressorvermogen in standen 0–8 (handmatig tot 12), en integreert volledig met Home Assistant via MQTT auto-discovery.



---

## Features

- **Vijf regelingsmodi** — Auto PID, Auto FF (feedforward), Water PID, Water FF, Handmatig
- **Feedforward controller** — model-gebaseerde stooklijn met online leren van UA_house en UA_emitter
- **Anna thermostaat support** — leest setpoint en kamertemperatuur via MQTT
- **Stooklijn compensatie** — aanvoertemperatuur omhoog bij vorst
- **Vorstbeveiliging** — automatisch bij instelbare grens
- **Koelmodus** — protocol byte 19-2,3 = 2, PID draait automatisch om
- **MQTT Auto-Discovery** — entities verschijnen automatisch in Home Assistant
- **LCD display** — 16x2 I2C statusscherm (optioneel)
- **EEPROM opslag** — instellingen blijven na herstart

---

## Regelingsmodi

### Auto modus — PID

PID regelt de aanvoertemperatuur op basis van de kamertemperatuurafwijking (Anna setpoint). De stooklijn berekent het doel-setpoint; de PID past het compressorvermogen (stand) aan.

```
Kamer gewenst: 20.5°C (Anna setpoint)

20.7°C ════ UIT trigger (hysteresis boven)
20.5°C ──── Doel 
20.4°C ════ AAN trigger (hysteresis onder)
```

Geoptimaliseerde PID parameters (KGE-gecalibreerd op historische data):
```
Kp = 19.9   Ki = 0.084   Kd = 0.036
```

---

### Auto modus — FF (Feedforward)

Model-gebaseerde controller die de benodigde aanvoertemperatuur berekent uit de warmtebalans van het huis:

```
T_supply = T_kamer_gewenst + (UA_house / UA_emitter_eff) × (T_kamer_gewenst − T_buiten)
```

De UA_house waarde wordt online bijgeleerd via een integraalterm op de kamerfout. Dit maakt de FF controller robuust voor veranderingen in isolatiewaarden en zonnewinst.

Geoptimaliseerde FF parameters (gecalibreerd op een goed geisoleerd vrijstaand houten huis):
```
ff_UA_house   = 272.5 W/K  (startwaarde, wordt online bijgesteld)
ff_UA_emitter = 267.5 W/K  (UA_eff afgifte-circuit)
FF_KI_AUTO    = 0.026       (integraalversterking kamertemperatuur)
FF_COAST_AUTO = 0.54°C      (anticipatiezone — stand omhoog als fout > 0.54°C)
```

Selecteer modus via MQTT: `chofu/cmd/modus = "ff_auto"`

---

### Water modus — PID

Regelt op een vast aanvoertemperatuur setpoint, ongeacht de kamertemperatuur. Nuttig als sturing op basis van OpenTherm of extern signaal gewenst is. In mijn geval  om de Adam zoneregeling te kunnen gebruiken

```
Setpoint + 1°C ══ UIT trigger
Setpoint        ── Doel 
Setpoint - 1°C ══ AAN trigger
```

Vermogensafbouw is geleidelijk (1 stand per 5 minuten) voor compressorbescherming.

Selecteer: `chofu/cmd/modus = "water"`

---

### Water modus — FF (Feedforward)

Model-gebaseerde variant die de benodigde compressorstand berekent vanuit de gewenste aanvoertemperatuur en de geleerde UA_emitter. De UA_emitter waarde wordt online bijgeleerd via een integraalterm op de aanvoerfout. Rustiger gedrag dan de PID-variant (minder stand-wisselingen).

Geoptimaliseerde FF parameters (KGE-gecalibreerd):
```
ff_UA_emitter  = 267.5 W/K  (startwaarde, wordt online bijgesteld)
FF_KI_WATER    = 0.017       (integraalversterking aanvoertemperatuur)
FF_COAST_WATER = 4.76°C      (anticipatiezone — stand omhoog als fout > 4.76°C)
```

Selecteer: `chofu/cmd/modus = "ff_water"`

---

### Handmatige modus

Vaste compressorstand 0–12 via MQTT. PID/FF niet actief.

```
Stand 0  =    0 W  (uit)
Stand 1  =  240 W
Stand 2  =  420 W
Stand 3  =  640 W
Stand 4  =  850 W
Stand 5  = 1050 W
Stand 6  = 1250 W
Stand 7  = 1450 W
Stand 8  = 1550 W  ← maximum in auto/water modus
─────────────────── alleen handmatig ───────────
Stand 9  = 1650 W
Stand 10 = 1700 W
Stand 11 = 1750 W
Stand 12 = 1800 W
```

---

### Koelmodus (nog niet getest)

Activeer via `chofu/cmd/koeling = "1"`. Koeling is **alleen beschikbaar** in `ff_auto`, `ff_water` en `handmatig` — niet in `auto` of `water`.

**FF_AUTO koeling** — feedforward op basis van warmte-inval van buiten:
```
P_nodig = UA_house × (t_outside − t_kamer_gewenst)
```
Regelt de compressorstand zodat de kamer op setpoint blijft.

**FF_WATER koeling** — feedforward op basis van kamer→water warmtestroom:
```
P_nodig = UA_emitter × (t_kamer − t_water_gewenst)
```
Regelt de aanvoertemperatuur op `t_water_gewenst` (bijv. 18°C).

**HANDMATIG** — vaste stand zoals bij verwarming.

**Condensatiebescherming:** aanvoer daalt nooit onder `SUPPLY_MIN` (default 17°C).  
**Afschakeldrempel:** koeling schakelt terug als kamer/aanvoer meer dan `KOELING_AFSCHAKEL` (0.5°C) onder setpoint zakt.  
**Seizoensbeveiliging:** koeling geblokkeerd als buiten < `KOELING_MIN_BUITEN` (default 18°C).

---

## Hardware

### Vereist
- **Arduino UNO R4 WiFi** — hoofdcontroller (of ESP32 als alternatief)
- **LCD 16×2 I2C Display** (optioneel) — statusscherm

### Benodigde Arduino bibliotheken
- `ArduinoMqttClient`
- `LiquidCrystal_I2C` (optioneel, alleen als LCD aangesloten)
- `Arduino_LED_Matrix` (alleen UNO R4 WiFi)
- WiFi is ingebouwd: `WiFiS3` op UNO R4 WiFi, `WiFi` op ESP32

---

## Bedrading

### Warmtepomp seriële verbinding


```
Arduino D0 (RX1) ←── Controlbox TX (pin 26)
Arduino D1 (TX1) ──→ Controlbox RX  (pin 27 via transistor/level shifter!)
Arduino GND      ───  Controlbox GND (pin 22)
```

> De transistor is nodig omdat de Chofu controlbox 5V logica gebruikt. Zie [WIRING.md](WIRING.md) voor het schema.

### LCD Display (optioneel)
```
VCC → 5V
GND → GND
SDA → A4
SCL → A5
```

---

## Installatie

### 1. Bibliotheken installeren
Arduino IDE → Sketch → Bibliotheek beheren:
- `ArduinoMqttClient` 
- `LiquidCrystal_I2C` 

### 2. Configuratie aanmaken
Kopieer het voorbeeldbestand en vul je eigen gegevens in:
```bash
cp chofu_wp_ff/config.h.example chofu_wp_ff/config.h
```
Bewerk `chofu_wp_ff/config.h`:
```cpp
const char* SSID        = "jouw-netwerk";
const char* PASS        = "jouw-wachtwoord";
const char* MQTT_BROKER = "192.168.1.x";
const int   MQTT_PORT   = 1883;
const char* MQTT_USER   = "";   // leeg als geen auth
const char* MQTT_PASS   = "";
```
`config.h` staat in `.gitignore` en wordt nooit gecommit.

### 3. Firmware uploaden
Open `chofu_wp_ff/chofu_wp_ff.ino` en upload naar Arduino UNO R4 WiFi.

### 3. Home Assistant
Entities verschijnen automatisch via MQTT discovery zodra de Arduino verbonden is. Geen YAML configuratie nodig.

Zie [INSTALLATION.md](INSTALLATION.md) voor uitgebreide stap-voor-stap instructies.

---

## MQTT Topics

### State (Arduino → HA)

| Topic | Omschrijving | Eenheid |
|-------|-------------|---------|
| `chofu/supply` | Aanvoertemperatuur | °C |
| `chofu/return` | Retourtemperatuur | °C |
| `chofu/outside` | Buitentemperatuur | °C |
| `chofu/kamer` | Kamertemperatuur (Anna) | °C |
| `chofu/kamer_gewenst` | Gewenste kamertemperatuur | °C |
| `chofu/stand` | Compressorstand (0–12) | — |
| `chofu/vermogen` | Geschat compressorvermogen | W |
| `chofu/comp_hz` | Compressorfrequentie | Hz |
| `chofu/delta_t` | Aanvoer − retour | °C |
| `chofu/modus` | Huidige modus | auto/ff_auto/water/ff_water/handmatig |
| `chofu/setpoint` | Actief aanvoer setpoint | °C |
| `chofu/doel_setpoint` | Actief regelsetpoint (stooklijn bij verwarming, kamer/water bij koeling) | °C |
| `chofu/water_setpoint` | Water modus setpoint | °C |
| `chofu/pid` | PID uitvoer | % |
| `chofu/ff_ua_house` | Geleerde UA_house (FF) | W/K |
| `chofu/ff_ua_emitter` | Geleerde UA_emitter (FF) | W/K |
| `chofu/koeling` | Koelmodus actief | 0/1 |
| `chofu/supply_min` | Condensatiegrens koeling | °C |
| `chofu/koeling_afschakel` | Afschakeldrempel koeling | °C |
| `chofu/defrost` | Ontdooien actief | 0/1 |
| `chofu/aan` | WP actief | 0/1 |
| `chofu/t_vorst` | Vorstbeveiligingsgrens | °C |
| `chofu/status` | Verbindingsstatus | online/offline |

### Commands (HA → Arduino)

| Topic | Omschrijving | Waarden |
|-------|-------------|---------|
| `chofu/cmd/modus` | Selecteer modus | `auto` / `ff_auto` / `water` / `ff_water` / `handmatig` |
| `chofu/cmd/stand` | Vaste stand (→ handmatig) | 0–12 |
| `chofu/cmd/water_setpoint` | Aanvoer setpoint water modus | 25–55°C |
| `chofu/cmd/setpoint` | Aanvoer setpoint auto stooklijn | 20–45°C |
| `chofu/cmd/koeling` | Koelmodus aan/uit | 0/1 |
| `chofu/cmd/supply_min` | Condensatiebescherming koeling (EEPROM) | 10–25°C |
| `chofu/cmd/koeling_afschakel` | Afschakeldrempel koeling | 0.1–5.0°C |
| `chofu/cmd/power` | WP aan/uit | 0/1 |
| `chofu/cmd/t_vorst` | Vorstgrens | −10 tot 10°C |
| `chofu/cmd/kp` / `ki` / `kd` | PID parameters | getal |
| `chofu/cmd/ff_ua_house` | FF UA_house overschrijven | W/K |
| `chofu/cmd/ff_ua_emitter` | FF UA_emitter overschrijven | W/K |
| `chofu/cmd/ff_save` | FF leerwaarden opslaan in EEPROM | 1 |
| `chofu/cmd/stooklijn_grens` | Buitentemp grens stooklijn | °C |
| `chofu/cmd/stooklijn_factor` | Stooklijn steilheid | 0.1–5.0 |
| `chofu/cmd/supply_max` | Max aanvoertemperatuur | °C |
| `chofu/cmd/force_start` | Hysteresis overbruggen | 1 |
| `chofu/cmd/hyst_slow` / `fast` / `down` | Hysteresistijden | ms |

### Subscriptions (Arduino leest)

| Topic | Omschrijving |
|-------|-------------|
| `anna/setpoint` | Anna thermostaat setpoint |
| `anna/temperatuur` | Anna kamertemperatuur |

### Log

| Topic | Omschrijving |
|-------|-------------|
| `chofu/log/INFO` | Normale events |
| `chofu/log/WARNING` | Waarschuwingen |
| `chofu/alert` | Persistente waarschuwing (retained) |

---

## Plugwise Adam integratie (`adam_api_test/`)

Experimentele integratie met de lokale REST API van de Plugwise Adam zoneregeling. De Adam bepaalt via OpenTherm de gewenste aanvoertemperatuur — deze kan als `t_water_gewenst` in de `water`/`ff_water` modus van de Chofu controller worden gebruikt.

| Bestand | Functie |
|---------|---------|
| `adam_api_test.ino` | Arduino/ESP32 sketch: haalt `intended_boiler_temperature` op (~1s, HTTP/1.1) |
| `adam_discover.py` | Python diagnostics: zones, keteldata en alle XML-types |
| `config.h.example` | Credentials template (kopieer naar `config.h`) |

**Werking:** `intended_boiler_temperature = 0` → geen warmtevraag; `> 0` → gewenste aanvoertemperatuur in °C.

---

## Python simulatietools

De map `python/` bevat tools voor het optimaliseren en valideren van regelparameters op historische Home Assistant data.

| Script | Functie |
|--------|---------|
| `replay_simulation.py` | Simuleer controller op historische CSV data |
| `optimize_all.py` | Optimaliseer PID en FF parameters (KGE objectief) |
| `calibrate_housemodel.py` | Kalibreer UA_house, C_th, UA_emitter op historische kamertemperatuur |

**Data-indeling:** exporteer via Home Assistant → Geschiedenis → CSV. Ondersteunt zowel het oude (entity/state) als het nieuwere formaat met `climate`-entiteiten en OpenTherm-sensoren.

---

## Troubleshooting

### Entities verschijnen niet in Home Assistant
- Wacht 2–3 minuten na opstarten
- Check of MQTT integratie geïnstalleerd is
- Controleer `chofu/status` topic in MQTT Explorer — moet "online" tonen
- Herstart Home Assistant als entities na integratie-reload verdwijnen

### WP gaat niet aan
- Hysteresis actief? (standaard 10 min)
- Override via: `chofu/cmd/force_start = 1`
- Check of modus correct is: `chofu/modus`

### Arduino bevriest na enige tijd
- Gebruik uitsluitend de hardware UART (Serial1, pins D0/D1) voor de WP-communicatie. SoftwareSerial conflicteert met de WiFi co-processor van de UNO R4 WiFi.

---

## Documentatie

- [docs/INSTALLATION.md](docs/INSTALLATION.md) — Installatie handleiding
- [docs/WIRING.md](docs/WIRING.md) — Bekabelingsschema's
- [docs/MQTT_REFERENCE.md](docs/MQTT_REFERENCE.md) — Volledige MQTT referentie
- [docs/TUNING.md](docs/TUNING.md) — PID en feedforward parameter tuning gids
- [docs/STOOKLIJN.md](docs/STOOKLIJN.md) — Stooklijn werking en instelling
- [docs/LCD.md](docs/LCD.md) — LCD scherm-indeling per modus
- [docs/ervaringen.md](docs/ervaringen.md) — Valkuilen en bevindingen uit de ontwikkeling

---

## Credits

**Protocol reverse engineering:** WackoH (Tweakers.net) — [Tweakers topic](https://gathering.tweakers.net/forum/list_messages/2220972/0)

**Bibliotheken:**
- ArduinoMqttClient (Arduino)
- LiquidCrystal_I2C (Frank de Brabander)

---

## Licentie

MIT License — zie [LICENSE](LICENSE) voor details.
