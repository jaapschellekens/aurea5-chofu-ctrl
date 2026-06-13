# Parameter Tuning Gids — ChofuCtrl

Deze gids beschrijft hoe je de PID- en feedforward-parameters aanpast, wat elk parameter doet en hoe je symptomen koppelt aan correcties. Alle parameters zijn instelbaar via MQTT zonder herflashen.

---

## Overzicht parameters per modus

| Modus | Regelaar | Stuurt op |
|-------|----------|-----------|
| `AUTO` | Cascade PID | Kamertemp (aan/uit) + aanvoertemp vs stooklijn (modulatie) |
| `WATER` | PID | Aanvoertemperatuur → vast water setpoint |
| `FF_AUTO` | Feedforward + integraal | Kamertemperatuur → UA_house model |
| `FF_WATER` | Feedforward + integraal | Aanvoertemperatuur → UA_emitter model |

---

## Deel 1 — PID parameters (AUTO en WATER)

### Hoe de PID werkt

AUTO werkt als een cascade:

```
Buitenste lus:  kamer_fout = t_kamer_gewenst − t_kamer
                → kamer_fout > AUTO_AAN_DREMPEL: WP aan, PID actief
                → kamer_fout < AUTO_UIT_DREMPEL: WP uit

Binnenste PID:  aanvoer_fout = doel_setpoint − t_supply
                (doel_setpoint = stooklijn op basis van buitentemp)

PID_output = Kp × aanvoer_fout
           + Ki × ∫aanvoer_fout dt
           + Kd × d(aanvoer_fout)/dt
           + dt_correctie      (corrigeert op delta_T aanvoer)
           + kamer_correctie   (boost als kamer nog ver van setpoint)

PID_output (0–100%) → stand (0–8) via tabel
```

WATER gebruikt dezelfde binnenste PID maar zonder buitenste kamer-lus:

```
water_fout = t_water_gewenst − t_supply   (vast setpoint, geen stooklijn)
```

### Parameters

**AUTO-modus** (stuurt op aanvoer vs stooklijn setpoint):

| Parameter | Default | Bereik | MQTT topic |
|-----------|---------|--------|------------|
| `Kp` — Proportionele versterking | 75.0 | 0.1–100 | `chofu/cmd/kp` |
| `Ki` — Integrale versterking | 0.800 | 0–2.0 | `chofu/cmd/ki` |
| `Kd` — Differentiële versterking | 0.010 | 0–10 | `chofu/cmd/kd` |

**WATER-modus** (stuurt op aanvoer vs vast water setpoint):

| Parameter | Default | Bereik | MQTT topic |
|-----------|---------|--------|------------|
| `Kp_water` | 50.0 | 0.1–100 | `chofu/cmd/kp_water` |
| `Ki_water` | 0.800 | 0–2.0 | `chofu/cmd/ki_water` |
| `Kd_water` | 0.010 | 0–10 | `chofu/cmd/kd_water` |

### Symptomen en correcties

| Symptoom | Oorzaak | Correctie |
|----------|---------|-----------|
| Aanvoer oscilleert (te warm/koud cyclus) | Kp te hoog | Kp verlagen (bijv. 75 → 50) |
| Aanvoer reageert traag op afwijking | Kp te laag | Kp verhogen |
| Blijvende afwijking na minuten | Ki te laag | Ki verhogen (bijv. 0.8 → 1.2) |
| Traag dalende maar lang doorgaande overshoot | Ki te hoog | Ki verlagen |
| Scherpe sprongen bij snelle setpointwijziging | Kd te hoog | Kd verlagen |
| Aanvoer rolt traag in op nieuw setpoint | Kd te laag | Kd verhogen |

### Hysteresistijden (gelden ook voor FF-modi)

| Parameter | Default | MQTT topic | Toelichting |
|-----------|---------|------------|-------------|
| `HYST_SLOW_MS` | 600 000 ms (10 min) | `chofu/cmd/hyst_slow` | Normale stap omhoog |
| `HYST_FAST_MS` | 120 000 ms (2 min) | `chofu/cmd/hyst_fast` | Urgente situatie (grote fout) |
| `HYST_DOWN_MS` | 60 000 ms (1 min) | `chofu/cmd/hyst_down` | Stap omlaag |

> Bij versnelde simulatie kun je deze tijden verkorten om sneller te testen:
> `chofu/cmd/hyst_slow` → `60000` (1 min), `hyst_fast` → `10000`, `hyst_down` → `10000`
> Zet ze daarna terug naar de defaults voor gebruik in de praktijk.

### PID interval

| Parameter | Default | MQTT topic |
|-----------|---------|------------|
| `pid_interval_ms` | 5000 ms | `chofu/cmd/pid_interval` |

---

## Deel 2 — Feedforward parameters (FF_AUTO en FF_WATER)

### Hoe het FF-model werkt

```
── FF_AUTO ──────────────────────────────────────────
P_nodig  = UA_house × (T_kamer_gewenst − T_buiten)
COP      = 0.40 × T_aanvoer_K / (T_aanvoer_K − T_buiten_K)   [1–6]
stand_ff = laagste stand waarbij VERMOGEN[s] >= P_nodig / COP

── FF_WATER ─────────────────────────────────────────
P_nodig  = UA_emitter × (T_water_gewenst − T_kamer)
COP      = 0.40 × T_water_K / (T_water_K − T_buiten_K)       [1–6]
stand_ff = laagste stand waarbij VERMOGEN[s] >= P_nodig / COP

── Gemeenschappelijk ────────────────────────────────
Als fout > FF_COAST:   stand_ff += 1   (extra boost)
Integraalcorrectie:    ±0–2 standen (trager, bij kleine fout)
```

### Parameters

| Parameter | Default | Bereik | MQTT topic | Modus |
|-----------|---------|--------|------------|-------|
| `ff_UA_house` | 272.5 W/K | 50–500 | `chofu/cmd/ff_ua_house` | FF_AUTO |
| `ff_UA_emitter` | 250.0 W/K | 50–500 | `chofu/cmd/ff_ua_emitter` | FF_WATER |
| `FF_COAST_AUTO` | 0.30°C | *(code)* | — | FF_AUTO |
| `FF_COAST_WATER` | 2.5°C | *(code)* | — | FF_WATER |
| `FF_KI_AUTO` | 0.026 | *(code)* | — | FF_AUTO |
| `FF_KI_WATER` | 0.017 | *(code)* | — | FF_WATER |

> `FF_COAST_*` en `FF_KI_*` staan in `types.h` en zijn niet via MQTT instelbaar. Bij aanpassing: firmware opnieuw uploaden én `EEPROM_MAGIC` verhogen.

### UA-waarden aanpassen via MQTT

```bash
# Directe overschrijving (niet permanent):
mosquitto_pub -t chofu/cmd/ff_ua_house   -m "272.5"
mosquitto_pub -t chofu/cmd/ff_ua_emitter -m "250.0"

# Opslaan in EEPROM (na instellen):
mosquitto_pub -t chofu/cmd/ff_save -m "1"
```

### Symptomen en correcties — FF_AUTO

| Symptoom | Diagnose | Correctie |
|----------|----------|-----------|
| Kamer te warm, WP draait te hard | `UA_house` te hoog | Verlagen (bijv. 272 → 250) |
| Kamer te koud, WP haalt setpoint niet | `UA_house` te laag | Verhogen (bijv. 272 → 300) |
| WP reageert traag bij grote koude snap | `FF_COAST_AUTO` te groot | Verlagen in `types.h` (→ 0.2°C) |
| WP "jaagt" — snel aan/uit | `FF_COAST_AUTO` te klein of `UA_house` ver van waarheid | UA bijstellen, eventueel HYST_SLOW verhogen |
| UA leert langzaam / nauwelijks | Weinig thermisch evenwicht bereikt | Normaal bij wisselend weer; wacht meerdere etmalen |

### Symptomen en correcties — FF_WATER

| Symptoom | Diagnose | Correctie |
|----------|----------|-----------|
| Aanvoer structureel te warm (+X°C) | `UA_emitter` te hoog | Verlagen (bijv. 267 → 250) |
| Aanvoer structureel te koud | `UA_emitter` te laag | Verhogen |
| Trage reactie na setpointstap (> 2 uur) | `FF_COAST_WATER` te groot | Verlagen in `types.h` (→ 2.0°C) |
| Oscillatie hoog-laag stand bij warme aanvoer | UA te hoog — overshoot — FF vraagt lagere stand | UA verlagen |
| Aanvoer bereikt setpoint maar UA stabiliseert niet | Weinig tijd in evenwicht (fout < 2°C) | Normaal; leert bij stabiel weer |

### Online leren — wat verwachten

Het model past `UA_house` en `UA_emitter` automatisch aan zolang de warmtepomp draait en het systeem dicht bij evenwicht is.

| Eigenschap | Waarde |
|------------|--------|
| Leersnelheid (`FF_LEARN_RATE`) | 0.0002 per 5s update |
| Tijdconstante | ~7 uur reëel |
| Leerconditie FF_AUTO | kamer_fout < 0.5°C |
| Leerconditie FF_WATER | aanvoer_fout < 2.0°C |

**Typisch verloop**: eerste 1–2 dagen stuurt de FF op basis van de startwaarden, daarna leert de UA-waarde in naar de werkelijke situatie. Te zien in HA als geleidelijke drift van `chofu/ff_ua_house` of `chofu/ff_ua_emitter`.

**Startwaarden opslaan** zodra de UA gestabiliseerd is:
```bash
mosquitto_pub -t chofu/cmd/ff_save -m "1"
```

---

## Deel 3 — FF_AUTO comfortparameters

Deze parameters bepalen hoe de regelaar omgaat met de aan/uit-cycli en overshoot van de kamertemperatuur. Ze zijn MQTT-instelbaar en worden **niet** in EEPROM opgeslagen (reboot herstelt de defaults).

### Stooklijn seizoenshysteresis

De warmtepomp schakelt uit als het buiten warm genoeg is en start niet direct opnieuw om kort cyclen te voorkomen.

| Parameter | Default | Bereik | MQTT topic | Omschrijving |
|-----------|---------|--------|------------|--------------|
| `STOOKLIJN_UIT_GRENS` | 18°C | 5–30°C | `chofu/cmd/stooklijn_uit` | Buitentemp waarbij WP volledig stopt |
| `STOOKLIJN_AAN_GRENS` | 16°C | 0–25°C | `chofu/cmd/stooklijn_aan` | Buitentemp waaronder WP weer mag starten (2°C hysteresis) |

> **Vuistregel**: kies `stooklijn_uit` gelijk aan de break-even buitentemp waarbij het huis geen verwarming meer nodig heeft. Voor een gemiddeld huis (UA_house ≈ 270 W/K, intern 150 W) is dat ~18°C. Stel `stooklijn_aan` = `stooklijn_uit` − 2°C.

| Symptoom | Correctie |
|----------|-----------|
| Kamer te koud in voor-/najaar (WP stopt te vroeg) | `stooklijn_uit` verhogen (bijv. 18 → 20°C) |
| WP draait onnodig bij milde temperaturen | `stooklijn_uit` verlagen (bijv. 18 → 16°C) |
| WP start/stopt kort na elkaar bij grenstemperatuur | `stooklijn_aan` verlagen (grotere hysteresis) |

### Terugschakelgedrag en herstart

| Parameter | Default | Bereik | MQTT topic | Omschrijving |
|-----------|---------|--------|------------|--------------|
| `FF_AFSCHAKEL_AUTO` | −0.5°C | −3.0–0.0°C | `chofu/cmd/ff_afschakel` | Kamer °C bóven setpoint waarbij terugschakelen begint |
| `FF_LOOKAHEAD_MS` | 5 min | 0–60 min | `chofu/cmd/ff_lookahead` | Vooruitkijktijd predictieve terugschakeling |
| `FF_MIN_OFF_MS` | 10 min | 0–120 min | `chofu/cmd/ff_min_off` | Min. uitschakelperiode na **seizoensstop** |
| `FF_THERMAL_MIN_OFF_MS` | 1 min | 0–30 min | `chofu/cmd/ff_thermal_min_off` | Min. uitschakelperiode na **thermische stop** (overshoot) |
| `FF_RESTART_COAST` | 0.20°C | 0.0–5.0°C | `chofu/cmd/ff_restart_coast` | Kamer °C ónder setpoint vereist voor herstart vanuit stand 0 |

#### Predictieve terugschakeling (`ff_lookahead`)

De regelaar extrapoleer de kamertemperatuur `ff_lookahead` minuten vooruit op basis van de gemeten stijgsnelheid over de laatste 20 minuten. Terugschakelen begint al als de **voorspelde** temperatuur de afschakeldrempel overschrijdt — ook als de actuele kamer nog op of onder het setpoint zit.

```
kamer_dt     = (T_kamer_nu − T_kamer_20min_geleden) / 1200s   [graden/s]
T_kamer_pred = T_kamer_nu + kamer_dt × ff_lookahead_s
Terugschakelen als: (setpoint − T_kamer_pred) < ff_afschakel
```

> Het 20-minuten afgeleide-venster past bij de thermische tijdconstante van het huis (tau ≈ 13 uur) en filtert meetruis eruit.

#### Thermische vs seizoensstop

| Stoptype | Oorzaak | Geldende min_off | `stooklijn_aan` blokkeert? |
|----------|---------|------------------|---------------------------|
| **Seizoensstop** | Buiten > `stooklijn_uit` | `FF_MIN_OFF_MS` (10 min) | Ja |
| **Thermische stop** | Kamer boven setpoint — stap naar stand 0 | `FF_THERMAL_MIN_OFF_MS` (1 min) | Nee |

#### Simulatieresultaten (buiten 10°C ±5°C, setpoint 20°C, 4 dagen)

| Configuratie | RMSE | Binnen ±0.5°C | Overshoot | Starts/dag |
|---|---|---|---|---|
| Origineel (uit=15°C, afschakel=−1.0, geen lookahead) | 0.56°C | 53% | +1.04°C | 3.3 |
| Stooklijn fix (uit=18°C, afschakel=−0.5) | 0.29°C | 92% | +0.56°C | 5.8 |
| + Lookahead 5 min | 0.28°C | 98% | +0.52°C | 6.0 |
| + Afschakel=−0.4, lookahead 5 min | 0.23°C | **100%** | +0.42°C | 7.0 |

> **Aanbeveling**: probeer `chofu/cmd/ff_afschakel` op `−0.4` voor maximaal comfort. Het enige nadeel is een iets hoger aantal starts per dag.

### Symptomen en correcties — FF_AUTO comfort

| Symptoom | Diagnose | Correctie |
|----------|----------|-----------|
| Kamer schiet 0.5–1°C bóven setpoint | Terugschakelen begint te laat | `ff_afschakel` richting 0 bijstellen (bijv. −0.5 → −0.3) |
| Kamer daalt 0.3–0.5°C ónder setpoint na uitschakelen | WP bleef te lang uit | `ff_restart_coast` verlagen of `ff_thermal_min_off` verkorten |
| WP start/stopt erg frequent (< 30 min cycli) | `ff_afschakel` te dicht bij 0 of `ff_restart_coast` te klein | `ff_afschakel` iets negatiever, `ff_restart_coast` iets groter |
| WP stopt lang bij milde buitentemp (overgang) | Seizoensstop + `ff_min_off` te lang | `stooklijn_uit` verhogen of `ff_min_off` verkorten |
| Kamer koud in voorjaar (WP stopt bij 15°C buiten) | `stooklijn_uit` te laag | Verhogen naar 18–20°C |

---

## Deel 4 — Vuistregels instellen bij een nieuw systeem

### Stap 1: Kies de juiste modus

| Situatie | Aanbevolen modus |
|----------|-----------------|
| Alleen radiators, geen slimme thermostaat | `WATER` of `FF_WATER` |
| Vloerverwarming + kamertemperatuursensor | `AUTO` of `FF_AUTO` |
| Plugwise Adam / extern water setpoint aanwezig | `FF_WATER` |
| Onbekend systeem, wil stabiel beginnen | `WATER` |

### Stap 2: Startwaarden bepalen

**UA_emitter (FF_WATER)**
Een schatting op basis van de radiatoren/vloervlakken. Vuistregel voor een gemiddeld huis: `UA_emitter ≈ 200–300 W/K`. Begin laag — onderschatting geeft minder overshoot dan overschatting.

**UA_house (FF_AUTO)**
Energielabelschatting: `UA_house = P_max / (T_binnen − T_buiten_design)`
Voorbeeld: 3 kW bij −10°C buiten, 20°C binnen → `UA_house = 3000/30 ≈ 100 W/K`.
Laat het leeralgoritme het verder verfijnen.

### Stap 3: Monitor via HA

| Entity | Wat zoek je |
|--------|------------|
| `chofu/ff_ua_house` | Stabiliseert binnen 1–2 dagen rond een vaste waarde |
| `chofu/ff_ua_emitter` | Idem — zakt als startwaarde te hoog was |
| `chofu/supply` vs `chofu/water_setpoint` | Tracking error < 2°C na inregelen |
| `chofu/stand` | Geen korte aan/uit-cycli (< 30 min) |

### Stap 4: Fijnafstelling

1. Kijk 3–5 dagen naar de data (of draai versnelde simulatie)
2. Als aanvoer structureel X°C te warm/koud → pas UA aan met `±X × UA / delta_T_aanvoer`
3. Als stand te snel wisselt → `HYST_SLOW_MS` verhogen
4. Als reactie op setpointwijziging te traag → `FF_COAST_WATER` verlagen in `types.h`
5. Als kamer structureel 0.5°C boven setpoint schiet → `ff_afschakel` richting 0 (bijv. −0.5 → −0.4)

---

## Deel 5 — Kalibratieparameters (niet aanpassen zonder hervalidatie)

De volgende waarden zijn geoptimaliseerd via KGE-kalibratie op simulatiedata en gelden voor de geconfigureerde hysteresisinstellingen. Aanpassen verstoort de balans:

```
AUTO:  Kp = 75.0   Ki = 0.800   Kd = 0.010
WATER: Kp = 50.0   Ki = 0.800   Kd = 0.010
FF_KI_AUTO  = 0.026        FF_COAST_AUTO  = 0.30°C
FF_KI_WATER = 0.017        FF_COAST_WATER = 2.5°C
```

---

## Referentie — alle instelbare MQTT-commando's

| Topic | Eenheid | Bereik | Effect |
|-------|---------|--------|--------|
| `chofu/cmd/kp` | — | 0.1–100 | PID proportioneel (AUTO) |
| `chofu/cmd/ki` | — | 0–2.0 | PID integraal (AUTO) |
| `chofu/cmd/kd` | — | 0–10 | PID differentieel (AUTO) |
| `chofu/cmd/kp_water` | — | 0.1–100 | PID proportioneel (WATER) |
| `chofu/cmd/ki_water` | — | 0–2.0 | PID integraal (WATER) |
| `chofu/cmd/kd_water` | — | 0–10 | PID differentieel (WATER) |
| `chofu/cmd/ff_ua_house` | W/K | 50–500 | FF_AUTO vermogensmodel |
| `chofu/cmd/ff_ua_emitter` | W/K | 50–500 | FF_WATER vermogensmodel |
| `chofu/cmd/ff_save` | — | `1` | Sla UA-waarden op in EEPROM |
| `chofu/cmd/hyst_slow` | ms | 100–3 600 000 | Hysteresis omhoog normaal |
| `chofu/cmd/hyst_fast` | ms | 100–3 600 000 | Hysteresis omhoog urgent |
| `chofu/cmd/hyst_down` | ms | 100–3 600 000 | Hysteresis omlaag |
| `chofu/cmd/pid_interval` | ms | 100–60 000 | PID/FF berekeningsinterval |
| `chofu/cmd/water_setpoint` | °C | 25–55 | Vast water setpoint (WATER-modus) |
| `chofu/cmd/stooklijn_basis` | °C | — | Basis stooklijn setpoint |
| `chofu/cmd/stooklijn_grens` | °C | 0–25 | Stooklijn startpunt buiten |
| `chofu/cmd/stooklijn_factor` | °C/°C | 0.1–5.0 | Stooklijn helling |
| `chofu/cmd/stooklijn_uit` | °C | 5–30 | Buitentemp waarbij WP seizoensstop |
| `chofu/cmd/stooklijn_aan` | °C | 0–25 | Buitentemp waarbij WP hervat na seizoensstop |
| `chofu/cmd/ff_afschakel` | °C | −3.0–0.0 | Kamer boven setpoint — terugschakelen (FF_AUTO) |
| `chofu/cmd/ff_lookahead` | min | 0–60 | Vooruitkijktijd predictieve terugschakeling |
| `chofu/cmd/ff_min_off` | min | 0–120 | Min. uitschakelperiode na seizoensstop |
| `chofu/cmd/ff_thermal_min_off` | min | 0–30 | Min. uitschakelperiode na thermische stop |
| `chofu/cmd/ff_restart_coast` | °C | 0.0–5.0 | Kamer onder setpoint vereist voor herstart |
| `chofu/cmd/auto_hyst_down` | min | 0–30 | Hysteresis afbouwen in AUTO-modus |
| `chofu/cmd/t_vorst` | °C | −10–10 | Vorstbeveiligingsdrempel |
| `chofu/cmd/supply_max` | °C | 40–80 | Noodstop aanvoer |
