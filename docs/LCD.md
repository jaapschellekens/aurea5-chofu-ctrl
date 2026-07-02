# LCD Schermen — ChofuCtrl

De LCD (16×2) roteert elke **6 s** door 3 schermen (+ een debug-scherm als
`proto_log` aan staat). Uitgangspunt: in één oogopslag zien **wat de WP doet**
(verwarmen / koelen / uit / tapwater / ontdooien) en de daarbij passende
temperatuur — zonder jargon-afkortingen.

---

## Scherm 1 — Wat doet hij nu (kern)

**Regel 1 = toestand + stand.** Het woord vat samen wat er gebeurt; de prioriteit
(hoog → laag):

| Voorwaarde | Regel 1 |
|---|---|
| `defrost` | `ONTDOOIEN` |
| `sww_actief` | `TAPWATER   st N` |
| WP uit (stand 0) | `UIT` |
| `koeling_modus` | `KOELEN     st N` |
| anders (verwarmen) | `VERWARMEN  st N` |

**Regel 2 = de geregelde temperatuur, passend bij de modus** (`>` = "naar doel"):

| Modus / situatie | Regel 2 | Toont |
|---|---|---|
| AUTO, FF_AUTO | `Kamer 20.1>21.0` | kamertemp → gewenst |
| WATER, FF_WATER | `Aanv 35.2>38.0` | aanvoer → doel-aanvoer |
| SWW actief | `Tapw 42.0>50.0` | aanvoer → SWW-setpoint |
| HANDMATIG | `Aanv 35  Ret 30` | aanvoer + retour (geen doel) |
| UIT | `Buiten 5.0 C` | rustige weergave |

Voorbeelden:

```
Verwarmen        Koelen           Tapwater         Uit
╔════════════════╗ ╔════════════════╗ ╔════════════════╗ ╔════════════════╗
║VERWARMEN  st 3 ║ ║KOELEN     st 2 ║ ║TAPWATER   st 5 ║ ║UIT             ║
║Kamer 20.1>21.0 ║ ║Aanv 18.5>16.0  ║ ║Tapw 42.0>50.0  ║ ║Buiten  5.0 C   ║
╚════════════════╝ ╚════════════════╝ ╚════════════════╝ ╚════════════════╝
```

Zo zie je meteen: verwarmt of koelt hij, op welke stand, en hoe ver de gemeten
temperatuur van het doel af zit.

---

## Scherm 2 — Omgeving & waterkring

```
╔════════════════╗
║Buiten 5.0 Hz45 ║   buitentemperatuur · compressorfrequentie
║Aanv 35  Ret 30 ║   aanvoer- en retourtemperatuur
╚════════════════╝
```

(Bij koelen kleurt niets, maar de getallen kloppen vanzelf: aanvoer < retour.)

---

## Scherm 3 — Systeem

```
╔════════════════╗
║192.168.1.50    ║   IP-adres (voor de web-interface)
║Modus: ff_water ║   volledige modusnaam
╚════════════════╝
```

---

## Scherm 4 — Protocol-debug (alleen als `proto_log` aan)

Ongewijzigd t.o.v. nu:

```
╔════════════════╗
║DBG jgc OK      ║   JGC-communicatie OK / TOUT (timeout)
║Aanv 35.2 Err:0 ║   aanvoer · aantal CRC-fouten
╚════════════════╝
```

---

## Wat verdwijnt t.o.v. de oude weergave

- De losse schermen met `PID:45% F:-0.9`, `UA:272`, `FF:+0.5`, `DT:`, `P:60%` —
  dat zijn afstel-/debugwaarden die thuishoren in de web-UI en HA, niet op het
  frontpaneel. (Blijven volledig beschikbaar via web/MQTT.)
- Van 5 schermen (soms 2 per modus) naar **3** (+ debug), dus een kortere,
  rustigere rotatie.

---

## Details

- Rotatie: Scherm 1 → 2 → 3 (→ 4 als `proto_log` aan), elke 6 s.
- Getallen die een regel delen (aanvoer+retour) staan op hele graden om binnen
  16 tekens te blijven; losse waarden met 1 decimaal.
- Het toestandwoord op regel 1 volgt deze prioriteit: ontdooien → tapwater → uit →
  koelen → verwarmen.

## LCD uitschakelen

Via MQTT: `chofu/cmd/lcd` → `0` (uit) / `1` (aan) — in HA de schakelaar *Chofu LCD*.
