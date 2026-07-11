# Plan: Plugwise Adam API-integratie in chofu_wp_ff

Status: **fase 1 geïmplementeerd** (branch `feature/adam-api`); fase 2 (multi-zone) open.
Basis: werkende proof-of-concept in `adam_api_test/` (streaming parser, basic auth).

## Doel

Optioneel, rechtstreeks vanuit de firmware:

1. **Watersetpoint** — `intended_boiler_temperature` van de Adam als `t_water_gewenst` (water/ff_water), inclusief koelsignalering.
2. **Kamertemperatuur** — één geconfigureerde zone als `t_kamer` (auto/ff_auto); alle overige zones alleen publiceren naar MQTT.

Zonder Adam-bron verandert er niets: MQTT (`chofu/cmd/water_setpoint`, `anna/temperatuur`) blijft werken zoals nu.

## Kernbeslissing: niet-blokkerend

De PoC blokkeert tot 15 s per fetch — onacceptabel naast de JGC-timing (TX ≤ 99 ms
na frame-einde). De module wordt daarom een **state machine** die per
`loop()`-doorgang een begrensd budget verwerkt (max ~256 bytes of 2 ms):

```
IDLE ──30s──▶ CONNECT ──▶ VERSTUUR ──▶ HEADERS ──▶ ZOEK_SP ──▶ LEES_SP
                                                        │
                                          ┌─────────────┘
                                          ▼
                                     ZOEK_ZONE ──▶ LEES_ZONE ──▶ KLAAR ──▶ IDLE
```

- `WiFiClient` levert bytes incrementeel; de bestaande `stream_find`/
  `stream_read_measurement`-logica wordt herschreven naar herstartbare varianten
  (matcher-index in state bewaren i.p.v. interne while-lus).
- Harde totaal-timeout per poll (20 s) → `ADAM_FOUT`, client sluiten, terug naar IDLE.
- Aparte socket naast MQTT (WiFiS3 ondersteunt meerdere).
- Nooit aangeroepen vanuit `lees_warmtepomp_data()` — alleen als aparte stap in `loop()`.

## Bestanden

| Bestand | Wijziging |
|---|---|
| `chofu_wp_ff/adam.h` / `adam.cpp` | Nieuw: state machine, parser, publieke API |
| `chofu_wp_ff/config.h.example` | `ADAM_IP`, `ADAM_PASS`, `ADAM_ZONE` (zonenaam voor t_kamer) |
| `chofu_wp_ff/globals.h/.cpp` | `adam_bron` (enum: UIT/WATER/WATER_KAMER), `adam_sp`, `adam_t_zone`, `adam_ok` |
| `chofu_wp_ff/chofu_wp_ff.ino` | `adam_poll();` in `loop()` |
| `chofu_wp_ff/regelaar.cpp` | Bronprioriteit + koelsignaal (zie onder) |
| `chofu_wp_ff/mqtt.cpp` | Commando, state-topics, discovery, alerts |
| `chofu_wp_ff/eeprom.cpp` | `adam_bron` persistent (magic byte ophogen!) |

## Publieke API van de module

```cpp
void  adam_poll();          // elke loop()-doorgang; doet zelf zijn timing (30s)
bool  adam_actief();        // bron != UIT en credentials aanwezig
bool  adam_geldig();        // laatste poll succesvol én < 90s oud
float adam_water_sp();      // intended_boiler_temperature (NAN indien ongeldig)
float adam_zone_temp();     // temperatuur van ADAM_ZONE (NAN indien ongeldig)
```

## Regelaar-integratie

Alleen actief als `adam_bron != UIT` **en** `adam_geldig()`:

### Watersetpoint (water / ff_water) — vervangt `chofu/cmd/water_setpoint`

| Adam SP | Betekenis | Actie |
|---|---|---|
| `0` | geen vraag | `t_water_gewenst = 0` → WP uit (bestaand pad, incl. vorstbeveiliging) |
| `10` | **koelvraag** (Anna geeft vast 10 °C) | `koeling_modus = true`, `t_water_gewenst = koel-setpoint` (nieuw instelbaar, default 18 °C, ondergrens SUPPLY_MIN) |
| `≥ 16` | verwarmingsvraag | `koeling_modus = false`, `t_water_gewenst = SP` (geclampt 16–55) |
| anders (10–16) | onbekend | negeren + eenmalige WARNING |

### Kamertemperatuur (auto / ff_auto, alleen bron WATER_KAMER)

`t_kamer = adam_zone_temp()` van de zone `ADAM_ZONE`; vervangt `anna/temperatuur`.
Anna-setpoint (`anna/setpoint`) blijft ongewijzigd de bron voor `t_kamer_gewenst`.

> **Let op:** in kamergestuurde modi ziet de regelaar alleen de geselecteerde zone.
> Warmtevraag in een ándere kamer wordt genegeerd — dezelfde beperking als met Anna
> als bron. Met Adam-zonering is `ff_water` daarom de aanbevolen hoofdmodus (de Adam
> doet de zone-arbitrage). Zie ook de optionele multi-zone-uitbreiding hieronder.

### Optioneel (fase 2): multi-zone vraagsturing in ff_auto

Regel op de zone met de **grootste warmtevraag** in plaats van één vaste zone.
Per zone staan zowel `temperature` als het `thermostat`-setpoint in de XML:

```
vraag_z   = setpoint_z − temp_z          (per zone, alleen zones met thermostaat)
actieve_z = zone met max(vraag_z)
t_kamer         = temp_actieve_z
t_kamer_gewenst = setpoint_actieve_z     (Anna-setpoint wordt genegeerd)
```

- Bronoptie: `adam_bron = multi` (naast `uit`/`water`/`water+kamer`).
- Parser leest per `<location>` dan ook het `thermostat`-setpoint (kleine uitbreiding,
  zelfde pass).
- Dempen van zone-wissels: actieve zone pas wisselen als een andere zone ≥ 0,3 °C
  méér vraag heeft (hysterese), anders pendelt de regelaar tussen zones.
- Alle zones onder hun setpoint → grootste (positieve) vraag wint; alle zones op
  temperatuur → vraag ≤ 0 → WP uit via bestaand pad.
- Kanttekening: de firmware kan alleen het **totale** vermogen regelen, niet de
  verdeling over zones — dat blijven de radiatorkranen/Adam doen. Multi-zone
  bepaalt dus alleen wannéér en hoe hard er gestookt wordt.

### Terugval

- `!adam_geldig()` (2 gemiste polls): terugvallen op de laatste MQTT-waarden +
  `stuur_alert("Adam API onbereikbaar — terug naar MQTT-bron")`. Éénmalig, niet spammen.
- De MQTT-watchdog en alle bestaande beveiligingen (SUPPLY_MIN, KOELING_MIN_BUITEN,
  vorst) blijven onverkort gelden — koelvraag van Adam wordt dus alsnog geblokkeerd
  als buiten < KOELING_MIN_BUITEN.

## MQTT

| Topic | Richting | Inhoud |
|---|---|---|
| `chofu/cmd/adam_bron` | HA→A | `uit` / `water` / `water+kamer` / `multi` (fase 2) (EEPROM) |
| `chofu/cmd/adam_koel_sp` | HA→A | koel-watersetpoint bij Adam-koelvraag (16–25, EEPROM) |
| `chofu/adam/bron` | A→HA | actieve bron |
| `chofu/adam/sp` | A→HA | laatste intended_boiler_temperature |
| `chofu/adam/status` | A→HA | `ok` / `fout` / `uit` |
| `chofu/adam/zone/<naam>` | A→HA | temperatuur per gevonden zone (max ~8, naam gesaneerd) |

Discovery: select (bron), number (koel_sp), sensor (sp, status, geconfigureerde zone).

## Parservolgorde in de XML (één pass, ~215 KB)

1. `<type>intended_boiler_temperature</type>` → `<measurement>` (~32 KB)
2. Daarna per `<location>`: `<name>` onthouden, dan eerste `<type>temperature</type>`-
   `<measurement>` binnen dat blok (zones vanaf ~43 KB)
3. Stream leeglezen tot einde of tot alle data binnen is → socket sluiten

Duur per poll: enkele seconden (verspreid, niet-blokkerend); interval 30 s.

## Randgevallen

- **Zonenaam niet gevonden** → `adam_zone_temp() = NAN` → kamerbron valt terug op Anna + eenmalige WARNING.
- **HTTP 401** → credentials fout → status `fout`, poll-interval verlengen naar 5 min.
- **XML-layoutwijziging** (firmware-update Adam) → parser vindt types niet → zelfde terugvalpad.
- **Herstart Adam** tijdens poll → socket-timeout → volgende poll herstelt.
- **RAM**: geen XML-buffering; alleen matcher-state + ~8 zonenamen à 24 tekens (~300 B).

## Testplan

1. Unit: parser voeden met opgeslagen XML-dump (via `adam_discover.py --raw`) in een host-side test of sim.
2. Op hardware, `adam_bron = uit`: verifiëren dat JGC-statistiek (`ok +N`, CRC ~0) gelijk blijft terwijl de module pollt (publiceren wel, regelen niet).
3. `adam_bron = water`: SP-volging + SP=0-gedrag; daarna koeltest (Anna op koelen → SP=10 → koeling aan).
4. `adam_bron = water+kamer`: zone-temperatuur vergelijken met Anna-waarde.
5. Uitvaltest: Adam van het netwerk halen → terugval + alert binnen 2 polls.
6. (Fase 2, `multi`) Vraag in één niet-geselecteerde kamer creëren → WP moet starten;
   zone-wissel-hysterese verifiëren (geen gependel tussen twee zones met bijna
   gelijke vraag).
