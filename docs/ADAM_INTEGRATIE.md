# Ontwerp — Optionele directe Plugwise Adam-integratie

Status: **geïmplementeerd** op branch `feature/plugwise-adam-integratie` (te testen op hardware)
Doel: de firmware kan setpoints en kamertemperatuur direct uit de Plugwise Adam
lokale REST API halen, zodat hij minder afhankelijk is van Home Assistant (MQTT).

> **Voor inschakelen op hardware:** draai eerst `adam_api_test/adam_discover.py`
> (evt. `--raw`) en (1) vul de exacte zonenamen in `config.h` (`ADAM_ZONES`),
> (2) verifieer of er een per-zone `control_state`/call-for-heat-veld bestaat —
> de parser-hook gebruikt nu de deficit-methode (`SP − temp`) als dat veld er niet
> is. De streaming-parser in `adam.cpp` is geschreven op de structuur uit
> `adam_discover.py` maar nog niet tegen een echte XML-dump gevalideerd.

### Bestanden (implementatie)

| Bestand | Wijziging |
|---|---|
| `adam.h` / `adam.cpp` | Nieuw: streaming pull-parser, leidende-zone, leer-gate, status |
| `config.h.example` | `USE_ADAM`, `ADAM_IP`, `ADAM_PASS`, `ADAM_ZONES` |
| `types.h` | `enum class Bron` + `bron_naar_str`/`str_naar_bron` |
| `globals.h/.cpp` | `bron`, `adam_leer_emitter_ok` |
| `protocol.h/.cpp` | `jgc_ontvangend()` accessor (mid-frame-guard) |
| `eeprom.h/.cpp` | `bron` persistent; `EEPROM_MAGIC` 0xB4→0xB5 |
| `mqtt.cpp` | `chofu/cmd/bron`, guards, modus-consistentie, status-publish |
| `regelaar.cpp` | leer-gate `ff_UA_emitter` bij bron==ADAM |
| `chofu_wp_ff.ino` | `adam_init()` in setup, `adam_poll()` in loop (beide `#if USE_ADAM`) |

---

## 1. Scope (deze versie)

| In scope | Uit scope (later) |
|---|---|
| Water-setpoint (`intended_boiler_temperature`) → `t_water_gewenst` | `auto` / `ff_auto` / `water` / `handmatig` met Adam-bron |
| Kamer-setpoint (zone `thermostat`) → `t_kamer_gewenst` | Automatische fallback Adam→MQTT |
| Actuele kamertemp (zone `temperature`) → `t_kamer`, via **leidende zone** (§4b) | Schrijven náár Adam (alleen lezen) |
| **Alleen `ff_water`-modus** gebruikt de Adam-bron | Onafhankelijk regelen per zone |
| Bron-keuze MQTT (default) ↔ Adam, **handmatig** schakelbaar | |

Belangrijk: in deze versie is de Adam-bron alleen actief/zinvol in **`ff_water`**.
In alle andere modi wordt de Adam-poll overgeslagen en blijven de MQTT-waarden leidend.

---

## 2. Bron-model: handmatig schakelen, MQTT default

Eén runtime-instelling bepaalt waar de drie waarden vandaan komen:

```
enum class Bron { MQTT, ADAM };   // default: MQTT
Bron bron = Bron::MQTT;
```

- **`MQTT` (default):** gedrag ongewijzigd. `t_water_gewenst`, `t_kamer_gewenst`,
  `t_kamer` worden gezet door de bestaande handlers
  (`chofu/cmd/water_setpoint`, `chofu/cmd/kamer_setpoint`, `chofu/cmd/kamer`).
  De Adam-poll draait niet.
- **`ADAM`:** alleen toegestaan als `modus == FF_WATER`. De Adam-poll vult de drie
  waarden; de genoemde MQTT-cmd-handlers worden genegeerd zolang `bron == ADAM`
  (anders zou HA er stilletjes overheen schrijven).

Schakelen gebeurt expliciet door de gebruiker — **geen** automatische omschakeling.

### Geen automatische fallback, wél vasthouden + alarmeren

Bij een mislukte Adam-fetch (NaN) blijven de **laatst geldige** waarden staan
(we overschrijven nooit met NaN). Na N opeenvolgende fouten (voorstel: 3, ≈90 s)
gaat er een alert naar `chofu/log/WARN` zodat het in HA zichtbaar is. De firmware
schakelt **niet** zelf terug naar MQTT — dat is een bewuste handeling van de
gebruiker (conform de keuze "handmatig schakelen"). Te overwegen optie voor later:
veiligheidsterugval naar `zet_uit()` na zeer lange uitval.

---

## 3. Besturing (nieuwe MQTT-topics + web)

| Topic | Richting | Waarde | Effect |
|---|---|---|---|
| `chofu/cmd/bron` | HA→fw | `mqtt` \| `adam` | Zet bron. `adam` alleen geaccepteerd als `modus==ff_water`, anders WARN-log + blijft `mqtt`. Persistent in EEPROM. |
| `chofu/bron` | fw→HA | `mqtt` \| `adam` | Actuele bron (retained), voor HA-discovery select/sensor. |
| `chofu/adam/status` | fw→HA | `ok` \| `fout` \| `uit` | Laatste fetch-resultaat. |
| `chofu/adam/laatste_ms` | fw→HA | getal | ms sinds laatste geslaagde fetch. |

Web-UI (`web.cpp`): één extra regel met de actuele bron + laatste Adam-status
(read-only volstaat voor deze versie; schakelen kan via MQTT).

Bij `chofu/cmd/modus`: als de nieuwe modus ≠ `ff_water` en `bron==ADAM`, dan
automatisch `bron=MQTT` forceren + WARN-log (consistente staat bewaken).

---

## 4. Parser-aanpak

Hergebruik de streaming-aanpak uit `adam_api_test.ino` (geen DOM — zie §6 geheugen).
Twee dingen moeten uit dezelfde HTTP-response (1 GET `/core/domain_objects`):

1. **Water-SP** — `<type>intended_boiler_temperature</type>` → eerstvolgende
   `<measurement>`. Staat op ~32 KB (~14 %). Werkt al.
2. **Zone temp + SP** — staan in `<location>`-elementen, **ongesorteerd**, vanaf
   ~43 KB. Per zone:
   - `point_log type=temperature` → actuele kamertemp
   - `point_log type=thermostat` → kamer-SP (point_log), of
   - `thermostat_functionality/setpoint` → kamer-SP (authoritatief, voorkeur)

Het discover-script `adam_api_test/adam_discover.py` toont exact deze velden en de
zonenamen — dat is de referentie voor de te matchen tags.

### Zone-identificatie (state-machine) — ALLE zones

> NB: t_kamer in een multizone-Adam is niet triviaal. Zie §4b voor de reden dat
> we **alle** zones moeten lezen en de leidende zone moeten kiezen.

Omdat zones niet gesorteerd zijn, kan de streaming-parser niet op volgorde
vertrouwen, en we kunnen níet na de eerste zone stoppen. State-machine die per
`<location>` een klein record vult in een vast array (≤8 zones):

```
zoek "<location"                         -> nieuw zone-record, binnen_location = true
  binnen location: lees <name>            -> zone.naam
    point_log temperature                 -> zone.temp
    point_log/func setpoint (thermostat)  -> zone.sp
    control_state / call-for-heat (§4b)   -> zone.vraagt   (indien aanwezig)
  bij "</location>": record vastleggen
... herhaal voor alle zones, plus water-SP onderweg ...
na laatste relevante data: kies leidende zone (§4b) -> t_kamer, t_kamer_gewenst
```

We lezen door tot zowel de water-SP als alle zone-records binnen zijn (of timeout).
In de praktijk staat het zone-blok vanaf ~43 KB; het tijdsbudget in §5 begrenst dit.

Robuustheid: per veld een `isnan()`-check; een ontbrekende zone of veld → die
waarde blijft op de oude (geen NaN-overschrijving). Let op de bekende valkuil:
een NaN passeert range-checks stil. Array overloopbeveiliging bij >8 zones
(extra zones negeren + WARN-log).

---

## 4b. Multizone: welke kamertemperatuur hoort bij de water-SP?

**Probleem.** De Adam levert één aanvoer-setpoint voor het hele hydraulische
systeem. Dat setpoint wordt op elk moment gedreven door de zone met de grootste
warmtevraag. In `ff_water` gebruikt de regelaar `t_kamer` op twee plekken:

- `P_nodig = ff_UA_emitter × (t_water_gewenst − t_kamer)`  (regelaar.cpp:245)
- leren: `ff_UA_emitter = P_hp / (t_supply − t_kamer)`       (regelaar.cpp:294)

Lees je `t_kamer` uit één vaste zone terwijl de water-SP door een andere zone
wordt gedreven, dan koppel je een setpoint van zone B aan de temperatuur van
zone A → verkeerde `P_nodig` en, erger, een vervuilde `ff_UA_emitter` (mengt
emittereigenschappen van meerdere kamers).

**Gekozen oplossing — leidende zone volgen + leer-gate.**

1. **`t_kamer` = temperatuur van de leidende zone** (die de water-SP drijft),
   zodat `t_kamer` consistent is met `t_water_gewenst`. `t_kamer_gewenst` =
   setpoint van diezelfde zone.
2. **Leidende zone bepalen:** als de XML een per-zone vraagsignaal heeft
   (`control_state` = heating / call-for-heat), gebruik dat. Anders terugvallen
   op grootste positieve `SP − temp`. Bij gelijkspel: hoogste deficit, dan
   stabiele tie-break op zonenaam.
3. **Leer-gate:** `ff_UA_emitter` mag alléén leren als de leidende zone
   **stabiel** dezelfde is — voorstel: ongewijzigd gedurende ≥ `FF_LEAD_STABIEL_MS`
   (bv. 10 min) én precies één zone leidend. Bij wisseling of meerdere
   gelijktijdige vragers: leren pauzeren (UA blijft schoon). Dit komt bovenop de
   bestaande `|regel_fout| < 2,0°C`-evenwichtsguard.

**Sprongen in `t_kamer`.** Bij wissel van leidende zone kan `t_kamer` springen.
Dat verstoort vooral `P_nodig` (feedforward); de integraal corrigeert dat omdat
`ff_water` primair de water-SP volgt (`regel_fout = wsp − t_supply`). Eventueel
later: lichte low-pass op `t_kamer` om sprongen te dempen — nu niet, eerst meten.

**Discover-stap vooraf.** Met `adam_api_test/adam_discover.py` vaststellen:
hoeveel zones er zijn, welke namen, en óf er een per-zone `control_state` /
call-for-heat-veld bestaat. Dat bepaalt of stap 2 het Adam-signaal of de
deficit-berekening gebruikt. (`--types` toont alle beschikbare `<type>`-waarden.)

**Geen warmtevraag.** Als geen enkele zone vraagt en/of water-SP = 0:
`ctrl.zet_uit()` — er is dan geen leidende zone nodig.

---

## 5. Niet-blokkerend pollen (vs JGC-RX)

Kernrisico (uit `ervaringen.md`): **geen blokkerende calls in het 666-baud
JGC-RX-pad** — een fetch van honderden ms tot seconden kan pompframes breken.

Maatregelen:

1. **Poll-interval** ~30 s (zoals de test), `bron==ADAM` én `modus==ff_water`.
2. **Alleen pollen als de JGC-parser niet mid-frame zit:** check
   `!jgc_is_ontvangend` vóór het starten van een fetch (dezelfde vlag die TX
   gate-t). Anders deze loop-iteratie overslaan.
3. **Tijdsbudget begrenzen:** harde `TIMEOUT_MS` (test: 15 s, voorstel verlagen
   naar ~3–4 s) en vroegtijdig `cl.stop()` zodra beide waarden binnen zijn, zodat
   de blokkade in de praktijk ~0,5–1 s blijft.
4. **Plaatsing in `loop()`:** de Adam-poll ná `lees_warmtepomp_data()` en buiten
   het kritieke JGC-venster. Bij twijfel: opsplitsen in een coöperatieve
   state-machine die per loop-iteratie een stukje van de stream leest (zwaarder,
   pas doen als de simpele variant frames blijkt te breken).

Aanbevolen eerste implementatie: simpele blokkerende fetch mét de mid-frame-guard
en kort tijdsbudget; meten in de praktijk (sniffer / `JGC timeout`-logs) of dit
frames kost. Pas opschalen naar de coöperatieve variant indien nodig.

---

## 6. Geheugen

- **UNO R4 (~32 KB RAM):** volledige XML (215 KB) DOM-parsen kan niet. Streaming
  state-machine met kleine vaste buffers (≤32 byte) is verplicht — geen `String`
  die meegroeit met de payload.
- **ESP32:** ruimer, maar zelfde streaming-aanpak voor uniforme code.
- Buffers: hergebruik `stream_find` / `stream_read_measurement` uit de test;
  voeg een korte naam-buffer toe voor zone-matching.

---

## 7. Modulestructuur

Nieuw, optioneel gecompileerd achter `#define USE_ADAM`:

```
chofu_wp_ff/
  adam.h     // API: void adam_init(); void adam_poll(); + extern status
  adam.cpp   // base64 + streaming parser (uit adam_api_test) + zone-state-machine
```

Inhaakpunten in bestaande bestanden:

- `chofu_wp_ff.ino`
  - include `"adam.h"` (binnen `#if USE_ADAM`).
  - in `loop()`: `#if USE_ADAM \n adam_poll(); \n #endif` (met guards uit §5).
- `globals.h/.cpp`: `Bron bron`, `t_*_adam` schaduwwaarden, `adam_*` statusvars.
- `mqtt.cpp`: handler `chofu/cmd/bron`; in de bestaande
  `water_setpoint`/`kamer_setpoint`/`kamer`-handlers een
  `if(bron==Bron::ADAM) return;`-guard; publiceer `chofu/bron` + `chofu/adam/*`;
  HA-discovery voor bron-select.
- `eeprom.cpp/.h`: `bron` persistent maken → **`EEPROM_MAGIC` ophogen**
  (huidige `0xB3` → `0xB4`) + nieuw ADDR / NVS-key `"bron"` (≤15 tekens).
- `config.h(.example)`: `#define USE_ADAM 1`, `ADAM_IP`, `ADAM_PASS`,
  `ADAM_ZONE "Woonkamer"`. (`SSID`/`PASS` bestaan al.)

Als `USE_ADAM` 0 is, compileert alles weg en is het gedrag identiek aan nu.

---

## 8. Datastroom (samenvatting)

```
modus == FF_WATER && bron == ADAM:
    [poll elke 30 s, niet mid-frame]
    GET /core/domain_objects  (HTTP/1.1, Basic smile:ADAM_PASS)
      -> intended_boiler_temperature       -> t_water_gewenst
      -> lees ALLE zones (temp, SP, vraag)
      -> kies leidende zone (§4b)
           leidende zone temperature       -> t_kamer
           leidende zone thermostat SP      -> t_kamer_gewenst
      -> leer-gate: ff_UA_emitter alleen leren bij stabiele leider (§4b)
    water-SP == 0 / geen vrager  -> ctrl.zet_uit()
    fetch faalt    -> oude waarden vasthouden; 3x fout -> WARN

anders (bron == MQTT, of modus != ff_water):
    ongewijzigd — waarden via chofu/cmd/* (HA)
```

---

## 9. Open punten voor de bouw

1. Discover via `adam_discover.py`: aantal zones, namen, en óf er een per-zone
   `control_state`/call-for-heat-veld is (bepaalt leider-detectie in §4b).
2. `FF_LEAD_STABIEL_MS` (leer-gate stabiliteitsvenster) — voorstel 10 min, meten.
3. Max aantal zones in het vaste array (voorstel 8) — afhankelijk van discover.
4. Definitief poll-tijdsbudget (3 s lijkt veilig; meten).
5. Web-UI: alleen tonen, of ook een schakelknop toevoegen?
6. Veiligheidsterugval bij langdurige Adam-uitval: niets / `zet_uit()` / auto→MQTT?
   (nu bewust "niets", handmatig).
7. Coöperatieve parser nodig, of volstaat blokkerend + mid-frame-guard? → meten.
8. Low-pass op `t_kamer` bij wissel leidende zone — nu niet, eerst meten.
