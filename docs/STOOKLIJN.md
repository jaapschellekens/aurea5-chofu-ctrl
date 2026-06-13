# Stooklijn

De stooklijn verhoogt automatisch de aanvoerwatertemperatuur naarmate het buiten kouder wordt. Dit compenseert het hogere warmteverlies van het huis bij vorst, zonder handmatige aanpassingen.

---

## Temperatuurschema — overzicht alle grenzen

### Aanvoertemperatuur (verticale as)

```
Aanvoer (°C)
    │
 60 ┤━━━━ SUPPLY_MAX ─────────────── veiligheidsplafond (alle modi)
    │      cmd: chofu/cmd/supply_max       nooit overschreden
    │
 45 ┤━━━━ Stooklijn cap ──────────── doel_setpoint nooit hoger (AUTO/FF_AUTO)
    │
    │      ↑ stooklijn actief gebied (t_buiten < STOOKLIJN_GRENS)
    │      doel = stooklijn_basis + (grens − buiten) × factor
    │
 ~38┤──── doel_setpoint bij 0°C buiten (standaardinstellingen)
    │
 32 ┤──── t_water_gewenst ────────── WATER / FF_WATER setpoint
    │      cmd: chofu/cmd/water_setpoint   (Adam of handmatig)
    │
 28 ┤━━━━ stooklijn_basis ────────── vlak deel stooklijn (AUTO/FF_AUTO)
    │      cmd: chofu/cmd/stooklijn_basis  (bij buiten ≥ STOOKLIJN_GRENS)
    │
 17 ┤━━━━ SUPPLY_MIN ─────────────── condensatiebescherming (koelmodus)
    │      cmd: chofu/cmd/supply_min       aanvoer nooit lager in koeling
    │
  0 ┤
```

### Buitentemperatuur — wanneer draait de WP?

```
Buiten (°C)
    │
+18 ┤━━━━ STOOKLIJN_UIT_GRENS ───── WP uit: stookseizoen voorbij (AUTO/FF_AUTO)
    │      cmd: chofu/cmd/stooklijn_uit
+16 ┤━━━━ STOOKLIJN_AAN_GRENS ───── WP hervat na uitschakelstop (AUTO/FF_AUTO)
    │      cmd: chofu/cmd/stooklijn_aan    2°C hysteresis → geen pendelen
    │
+15 ┤━━━━ STOOKLIJN_GRENS ─────────stooklijn begint aanvoer verhogen
    │      cmd: chofu/cmd/stooklijn_grens  (boven deze grens: vlak)
    │
    │      (stooklijn actief gebied)
    │
 +2 ┤━━━━ T_VORST ─────────────────vorstbeveiliging: WP verplicht op stand 1
    │      cmd: chofu/cmd/t_vorst          ook als kamer al op temp
    │
  0 ┤
    │
-10 ┤──── stooklijn max ──────────── doel_setpoint = 45°C (begrensd)
    │
```

### Water setpoint validatie (WATER / FF_WATER)

```
water_setpoint (°C)
    │
 55 ┤━━━━ maximum ──────── boven 55: genegeerd
    │
    │      (geldig gebied: 16–55°C)
    │
 16 ┤━━━━ WATER_SP_MIN ─── onder min én ≠ 0: genegeerd
    │      cmd: chofu/cmd/water_sp_min     (beschermt tegen Adam-glitches)
    │
  1 ┤──── GENEGEERD ─────── 1 t/m (min-1): ongeldig, geen effect
    │
  0 ┤━━━━ Geen warmtevraag ─ WP uit (vorstbeveiliging blijft)
    │      bijv. Adam stuurt 0 bij geen warmtevraag
```

---

## Welke parameters gelden per modus?

| Parameter | AUTO | FF_AUTO | WATER | FF_WATER | HAND |
|-----------|:----:|:-------:|:-----:|:--------:|:----:|
| `stooklijn_basis` (basis aanvoer) | ✅ | ✅ | — | — | — |
| `STOOKLIJN_GRENS` (curve start) | ✅ | ✅ | — | — | — |
| `STOOKLIJN_FACTOR` (hellingshoek) | ✅ | ✅ | — | — | — |
| `STOOKLIJN_UIT_GRENS` (seizoensstop) | ✅ | ✅ | — | — | — |
| `STOOKLIJN_AAN_GRENS` (hervat) | ✅ | ✅ | — | — | — |
| `T_VORST` (vorstbeveiliging) | ✅ | ✅ | — | — | — |
| `t_water_gewenst` (water setpoint) | — | — | ✅ | ✅ | — |
| `WATER_SP_MIN` (min. water setpoint) | — | — | ✅ | ✅ | — |
| `SUPPLY_MAX` (max. aanvoer) | ✅ | ✅ | ✅ | ✅ | ✅ |
| `SUPPLY_MIN` (min. aanvoer koeling) | ✅ | ✅ | ✅ | ✅ | ✅ |

---

## Instellen: beslisboom per modus

### AUTO / FF_AUTO

```
Stap 1: Stel STOOKLIJN_UIT_GRENS in
  → Boven welke buitentemperatuur heeft het huis geen verwarming nodig?
     Typisch 15–18°C. Zet STOOKLIJN_AAN_GRENS ~2°C lager.

Stap 2: Stel stooklijn_basis in
  → Welke aanvoertemperatuur is nodig bij mild weer (10–15°C buiten)?
     Bij vloerverwarming typisch 25–32°C, bij radiatoren 35–45°C.

Stap 3: Stel STOOKLIJN_GRENS in
  → Onder welke buitentemperatuur moet de aanvoer omhoog?
     Typisch 10–15°C (= zachtste dag waarop je merkt dat het te koud is).

Stap 4: Stel STOOKLIJN_FACTOR in
  → Hoeveel graden aanvoer extra per °C buiten koeler?
     Begin met 0.68; verhoog (bijv. 0.85) als het huis koud blijft bij vorst,
     verlaag als de aanvoer te snel stijgt.

Stap 5: Stel T_VORST in
  → Onder welke buitentemperatuur moet de WP altijd aan (vorstbescherming)?
     Default 2°C is veilig voor de meeste installaties.
```

### WATER / FF_WATER

```
Stap 1: Stel WATER_SP_MIN in
  → Wat is de laagste zinnige aanvoertemperatuur voor je installatie?
     Typisch 16–20°C. Waarden van de Adam tussen 1 en deze grens worden genegeerd.

Stap 2: Configureer de Adam
  → Stuur 0 bij geen warmtevraag (WP uit) en een geldig setpoint (≥ WATER_SP_MIN)
     bij warmtevraag. De controller neemt het setpoint direct over.

Stap 3: SUPPLY_MAX eventueel aanpassen
  → Stel in op de maximale aanvoertemperatuur die je installatie verdraagt.
     Default 60°C is veilig voor de meeste warmtepompen.
```

---

## Hoe het werkt

De controller berekent een **doel-aanvoertemperatuur** (`doel_setpoint`) op basis van de buitentemperatuur:

```
doel_setpoint = setpoint + (STOOKLIJN_GRENS − t_buiten) × STOOKLIJN_FACTOR
                                ↑ alleen toegepast als t_buiten < STOOKLIJN_GRENS
```

Het resultaat wordt begrensd op **45°C**.

### Voorbeeld met standaardinstellingen

| Parameter | Standaardwaarde |
|-----------|----------------|
| `setpoint` (basis aanvoertemperatuur) | 28°C |
| `STOOKLIJN_GRENS` (startpunt verhoging) | 15°C |
| `STOOKLIJN_FACTOR` (hellingshoek) | 0.68 °C/°C |

| Buitentemperatuur | Doel-aanvoertemperatuur |
|---|---|
| 20°C | 28.0°C — geen verhoging, verwarming uit (boven `STOOKLIJN_UIT_GRENS`) |
| 15°C | 28.0°C — vlak (precies op drempel) |
| 10°C | 31.4°C |
|  5°C | 34.8°C |
|  0°C | 38.2°C |
| −5°C | 41.6°C |
| −7°C | 43.2°C |
| −10°C | 45.0°C — begrensd |

---

## Stookkransen: wanneer loopt de WP?

### `STOOKLIJN_UIT_GRENS` — Uitschakelgrens (standaard: 18°C)

Als de buitentemperatuur **boven** deze waarde stijgt, wordt de warmtepomp volledig uitgeschakeld. Het stookseizoen is voorbij. Geldt voor `AUTO` en `FF_AUTO`.

### `STOOKLIJN_AAN_GRENS` — Inschakelgrens (standaard: 16°C)

Na een uitschakelstop (boven `STOOKLIJN_UIT_GRENS`) herstart de warmtepomp pas als de buitentemperatuur **onder** deze waarde daalt. De 2°C hysteresis voorkomt snelle aan/uit-cycli bij temperaturen rond de uitschakeldrempel.

### `T_VORST` — Vorstbeveiliging (standaard: 2°C)

Als de buitentemperatuur **onder** deze waarde daalt, wordt de warmtepomp gedwongen ingeschakeld op minimumstand (stand 1), ook als de kamer al op temperatuur is. Voorkomt bevriezing van de leidingen. Geldt voor `AUTO` en `FF_AUTO`.

---

## Gebruik per regelingsmodus

### `AUTO` — Kamer-PID

De stooklijn levert het **PID-setpoint**. De PID-regelaar vergelijkt de berekende `doel_setpoint` met de werkelijke aanvoertemperatuur en stuurt de compressorstand bij.

```
doel_setpoint = stooklijn-resultaat
aanvoer_fout  = doel_setpoint − t_aanvoer   ← PID-fout
```

Zonder stooklijn zou de PID bij iedere buitentemperatuur dezelfde aanvoertemperatuur nastreven, wat bij vorst onvoldoende zou zijn.

---

### `FF_AUTO` — Feedforward op kamertemperatuur

De stooklijn wordt berekend en gebruikt als **actief setpoint** (`wsp = stooklijn`). De feedforward-regelaar stuurt de compressorstand vervolgens bij op basis van het warmtebalansmodel van het huis:

```
P_nodig = UA_house × (t_kamer_gewenst − t_buiten)
COP     = 0.40 × (T_aanvoer_K / (T_aanvoer_K − T_buiten_K))
stand   = laagste stand waarbij VERMOGEN[stand] ≥ P_nodig / COP
```

De `UA_house`-waarde wordt online bijgeleerd, waardoor deze modus zichzelf calibreert. De stooklijn bepaalt hier de *doeltemperatuur* voor het afschakelbesluit (te warm = uit); de feedforwardfysica stuurt de werkelijke stand.

> **Tip:** In `FF_AUTO` is het effectiever om `ff_UA_house` bij te sturen dan de stooklijnparameters.

---

### `WATER` — Aanvoer-PID op vast setpoint

De stooklijn wordt **niet gebruikt**. De PID regelt de aanvoertemperatuur op een vaste waarde (`t_water_gewenst`, standaard 32°C), ongeacht de buitentemperatuur.

---

### `FF_WATER` — Feedforward op aanvoertemperatuur (Adam-setpoint)

De stooklijn wordt **niet gebruikt**. In plaats daarvan volgt de controller het **extern opgelegde watertemperatuursetpoint van de Plugwise Adam** (`t_water_gewenst`). De feedforward berekent het benodigde compressorvermogen op basis van het emittermodel:

```
P_nodig = UA_emitter × (t_water_gewenst − t_kamer)
COP     = ff_cop(t_water_gewenst, t_buiten)
stand   = laagste stand waarbij VERMOGEN[stand] ≥ P_nodig / COP
```

De `UA_emitter`-waarde wordt online bijgeleerd. Het setpoint komt volledig van de Adam — de controller volgt de stooklijn van het Adam-systeem in plaats van een eigen.

---

### `HANDMATIG` — Vaste stand

Niet van toepassing. De compressor draait op een vaste stand ingesteld via `chofu/cmd/stand`.

---

## Overzichtstabel

| Modus | Stooklijn gebruikt? | Rol |
|-------|---------------------|-----|
| `AUTO` | ✅ Ja | PID-setpoint (doel-aanvoertemperatuur) |
| `FF_AUTO` | ✅ Ja | Afschakelbesluit + doeltemperatuur voor FF |
| `WATER` | ❌ Nee | Vast `t_water_gewenst` |
| `FF_WATER` | ❌ Nee | Extern Adam-setpoint (`t_water_gewenst`) |
| `HANDMATIG` | ❌ Nee | — |

---

## Stooklijn instellen

Alle parameters worden **direct in EEPROM opgeslagen** na een wijziging — ze overleven een herstart. Stuur de waarde als getal (bijv. `0.85`) naar het MQTT-commandotopic.

| MQTT-commandotopic | Parameter | Standaard | Bereik | Wat het bepaalt |
|--------------------|-----------|-----------|--------|-----------------|
| `chofu/cmd/stooklijn_basis` | `setpoint` | 28°C | — | Basis-aanvoertemperatuur (vlak deel van de curve, bij en boven `STOOKLIJN_GRENS`) |
| `chofu/cmd/stooklijn_grens` | `STOOKLIJN_GRENS` | 15°C | 0–25°C | Buitentemperatuur waaronder de verhoging begint |
| `chofu/cmd/stooklijn_factor` | `STOOKLIJN_FACTOR` | 0.68 | 0.1–5.0 | °C aanvoerverhoging per °C onder de drempel |
| `chofu/cmd/stooklijn_uit` | `STOOKLIJN_UIT_GRENS` | 18°C | 5–30°C | Buitentemperatuur waarboven de verwarming stopt |
| `chofu/cmd/stooklijn_aan` | `STOOKLIJN_AAN_GRENS` | 16°C | 5–30°C | Buitentemperatuur waaronder de verwarming hervat (hysteresis t.o.v. `stooklijn_uit`) |
| `chofu/cmd/water_setpoint` | `t_water_gewenst` | 32°C | 25–55°C | Vast aanvoersetpoint voor `WATER`-modus; in `FF_WATER` overgenomen van de Adam |

Actuele waarden worden gepubliceerd (zonder retain) op:
`chofu/doel_setpoint`, `chofu/stooklijn_grens`, `chofu/stooklijn_factor`, `chofu/stooklijn_uit`, `chofu/stooklijn_aan`, `chofu/water_setpoint`

Dezelfde parameters zijn ook instelbaar via de ingebouwde **webinterface** (poort 80 op het IP-adres van de Arduino).

### Home Assistant

De stooklijnparameters verschijnen automatisch als *number*-entiteiten in Home Assistant na MQTT auto-discovery. Zoek naar **Chofu Stooklijn Grens**, **Chofu Stooklijn Factor** en **Chofu Stooklijn Uit** in het Chofu-apparaat.

---

## Afsteltips

**Huis te koud bij lage buitentemperaturen:**
→ Verhoog `STOOKLIJN_FACTOR` (bijv. 0.68 → 0.85). De curve wordt steiler.

**Huis schiet door op matige dagen (bijv. 8–12°C buiten):**
→ Verlaag `STOOKLIJN_GRENS` (bijv. 15 → 10°C) zodat de verhoging alleen bij echte vorst actief is.

**WP loopt onnodig in voor- en najaar:**
→ Verlaag `STOOKLIJN_UIT_GRENS` (bijv. 15 → 12°C).

**Vlak deel van de curve te hoog of te laag:**
→ Pas `setpoint` aan (basis-aanvoertemperatuur).

**`FF_AUTO` en kamertemperatuur wijkt structureel af:**
→ Stuur `ff_UA_house` bij via `chofu/cmd/ff_ua_house` — het feedforwardmodel is directer en leert zichzelf bij.

**`FF_WATER` volgt een verkeerde watertemperatuur:**
→ Controleer het setpoint dat de Plugwise Adam publiceert op `chofu/water_setpoint`. De controller neemt dit setpoint volledig over.
