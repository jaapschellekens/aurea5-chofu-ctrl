# Analyse: OpenTherm-link van de controlbox lezen én schrijven

Status: **fase 1 (RX-only) gebouwd** op branch `feature/ot-link`.
Bronnen: `JGC/jgc.ino` (JHN) én — sterker — het chip-dump-verslag
`data/atlantic` (disassembly van de originele M1.1/T1.1, juli 2026).
**Naamgeving gecorrigeerd n.a.v. het verslag:** M1.1 = IC1 = de verwijderde
pomp-chip (die wij vervangen); T1.1 = IC2 = de OpenTherm-brug die blijft zitten.

## 1. Wat is "de OpenTherm-communicatie" hier precies?

**Niet** de ruwe OpenTherm-bus (17V stroomlus, Manchester-encoding). In de Aurea-controlbox
zitten twee MCU's:

```
Anna ──OT (T1.1=slave)──┐
                         ├─ T1.1 (IC2, OT-brug) ←──9600 baud TTL──→ M1.1 (IC1, VERWIJDERD)
Gasketel ──OT (T1.1=master)┘                                            │
                                                                666 baud → warmtepomp
```

- **T1.1 (IC2)** is een tweezijdige OpenTherm-brúg: slave richting Anna, master richting
  de ketel. Alle warmtevraag loopt er fysiek doorheen; hij herschrijft onderweg vijf
  Data-ID's (o.a. ID25/ID28: Anna krijgt de wármtepomp-temperaturen te zien i.p.v. de
  koude ketel). Hij blijft op het bord zitten.
- **M1.1 (IC1)** is de chip die wij verwijderd hebben. Onze Arduino bedient zijn
  pomp-kant (666 baud), maar nog niet zijn T1.1-kant (9600 baud) — daardoor ziet T1.1
  een timeout, gaat de rode storingsled aan en stuurt hij warmtevraag naar de gasketel.

Het aan te sluiten signaal is dus een gewone **TTL-UART op 9600 baud, 8N1**, op de
vrijgekomen pads van het IC-voetje ("pad 1" = RX van IC1, "pad 2" = TX naar IC1 in
JHN's notatie; in de praktijk één full-duplex UART).

## 2. Protocol (geverifieerd via chip-dump `data/atlantic` + jgc.ino)

**Framing:** vaste **12 bytes**, 9600 8N1 (UBRR-berekening in de binary bevestigt dit),
checksum = som van bytes 0–10 mod 256 in byte 11. Geen CRC.

**T1.1 → Arduino** (wat wij in fase 1 uitlezen):

| Byte | Betekenis | Bron |
|---|---|---|
| 0 | **Gewenst aanvoersetpoint van Anna** (°C, integer; Anna's Control Setpoint) | jgc.ino — fase 1-hexdumps (`chofu/ot/raw`) moeten dit bevestigen |
| 1–10 | Onbekend | te reverse-engineeren uit de dumps |
| 11 | Checksum | bevestigd |

**Arduino → T1.1** (fase 2 — indeling nu uit de binary bevestigd, dit is wat M1.1 stuurde):

| Byte | Betekenis | Bron |
|---|---|---|
| 0 | Kopbyte, **vast `0xF0`** | binary (T1.1 leest vanaf index 6) |
| 7 | `round(T_supply)` — belandt via OT ID25 in Anna | binary |
| 8 | `round(T_return)` — via OT ID28 naar Anna | binary |
| 9 | `round(T_outside)` | binary |
| 10 | **Statuscode: `0` = OK** (M1.1 kende alleen 0, 0x01=timeout, 0x64=vastgelopen). T1.1 vertaalt ≠0 naar het knipperpatroon van de storingsled — **status 0 sturen dooft dus de rode knop** | binary |
| 1–6 | Onbekend; jgc-template (20, 154, 0, 0, 0, 0) aanhouden | jgc.ino |
| 11 | Checksum | bevestigd |

> **Correctie op jgc.ino:** het `0xF0`-in-byte-0 dat jgc als "CV moet aan"-commando
> interpreteerde is volgens de binary gewoon de vaste kopbyte van élk M1.1-telegram.
> De bijstook-beslissing zit niet in dit veld maar in T1.1 zelf (drempel op Anna's
> Control Setpoint, ±20 °C) — zie §3b.

**Timing:** antwoord ±200 ms na een ontvangen frame; **maximaal 4,2 s** tussen
zendbeurten (keepalive — anders beschouwt IC1 de link als dood → storing).
Ruim onkritisch vergeleken met de pomp-link (99 ms).

**Hybride logica.** Uit de binary: T1.1 beslist zélf over de ketel (OpenTherm-master):
onder een drempel op Anna's Control Setpoint (~20 °C) krijgt de ketel een pro-forma
setpoint van 10 °C ("blijf uit"), erboven een echte. M1.1 had bovendien een **harde
5 °C-lockout** op de buitentemperatuur (health-teller → vraagrelais uit) — het
fabrieks-bivalentiepunt. Onze firmware heeft die lockout bewust niet; ketel-bijstook
kan in fase 3 via de buitentemperatuur in byte 9 beïnvloed worden (jgc stuurde daar 4
om CV-start te blokkeren) — exact gedrag te verifiëren in fase 2.

> **Let op (correctie op eerdere thread-info):** de ketel wordt volgens de binary
> **niet** via een relais maar via OpenTherm aangestuurd. "Pin 16 = ketelrelais" uit
> de Tweakers-thread is dus twijfelachtig (pin 16 = PB2, volgt modusingang PC2);
> eerst doormeten voordat we er iets mee doen.

## 3. Wat levert het op

1. **Controlbox uit storing** — de rode knop dooft, geen ongewenste gasketel-start meer
   (het huidige workaround "ketel op alleen-tapwater" kan vervallen).
2. **Anna-setpoint direct** — `t_water_gewenst` zonder Adam-API of MQTT-omweg
   (byte 0 = zelfde waarde als `intended_boiler_temperature`; vermoedelijk óók de
   koelconventie SP=10).
3. **Echte hybride werking** — bij extreme kou kan de firmware bewust de gasketel
   inschakelen (`0xF0`), zoals het origineel bedoeld was.
4. **WP-temperaturen zichtbaar in Anna/Plugwise** (bytes 7/8/9 gaan naar OpenTherm).

## 4. Hardware: waar halen we de extra UART vandaan?

De UNO R4 WiFi heeft maar één vrije hardware-UART (`Serial1`) en die is bezet door de
pomp (timing-kritisch, blijft daar). Opties:

| Optie | Oordeel |
|---|---|
| **A. SC16IS750 I²C-UART-bridge** (~€4 breakout) | **Aanbevolen voor de R4.** Hangt aan de bestaande I²C-bus (A4/A5, naast het LCD, ander adres). 64-byte FIFO — een 12-byte frame op 9600 baud (~12,5 ms) past ruim; pollen per loop-doorgang volstaat, geen interrupt nodig. Let op: de chip is 3,3 V — **level shifter (2-kanaals, ~€1) tussen bridge en de 5V-pads** van het voetje. |
| B. ESP32 i.p.v. UNO R4 | De firmware compileert al voor ESP32 (3 UARTs: pomp op UART2, OT-link op UART1). Geen extra chips, wél: board wisselen, LED-matrix vervalt, en de ESP32 is 3,3 V → level shifters voor **beide** links. Goede optie bij een toch geplande board-wissel. |
| C. Tweede SCI van de RA4M1 activeren | De chip heeft meer UARTs maar de Arduino-core stelt ze niet beschikbaar; FSP-gehack, breekt bij core-updates. Afgeraden. |
| **D. SoftwareSerial (te testen — eerst RX-only)** | De officiële R4-core (≥1.6) levert een SoftwareSerial op basis van **GPT-timers + DMA** (geen klassiek bit-bangen met interrupts uit). De eerdere crash-ervaring stamt uit oude code met een third-party port. Goedkoopste optie (0 extra hardware). Testprotocol: aparte branch, RX-only op D8, ≥1 dag draaien met drie afbreekcriteria: `JGC (30s)`-statistiek verandert niet, MQTT blijft stabiel, geen reboots. Faalt één criterium → optie A. Let op: deelt GPT-timers met de LED-matrix. |

**Bekabeling (optie A):**

```
IC-voetje pad "IC1-TX" (pad 1) ──[level shift]──→ SC16IS750 RX
IC-voetje pad "IC1-RX" (pad 2) ←─[level shift]──── SC16IS750 TX
GND gedeeld (al aanwezig) · SC16IS750: VCC 3.3V, SDA→A4, SCL→A5, adres ≠ 0x27 (LCD)
```

### Pinnen van het 28-pins IC-voetje (uit de Tweakers-thread)

| Pin | Functie |
|---|---|
| 2 en 3 | **Communicatie met IC1 (OT-controller)** — onze RX/TX-pads |
| 7 | 5 V (voeding; iemand voedde er een ESP32 mee) |
| 8 | GND |
| 16 | **Ketel-aan** (relais schakelt) — mogelijke directe gasketel-sturing, zie onder |
| 26 | TX naar WP (in gebruik: → Arduino D1 via 1 kΩ) |
| 27 | RX vanaf WP (in gebruik: → Arduino D0) |

Welke van pin 2/3 de IC1-TX is (onze RX) nog meten: de pad die in rust ~5 V voert en
elke ~1–4 s korte activiteit toont wordt door IC1 gedreven → via 10 kΩ naar `OT_RX_PIN`.
De andere is onze (fase 2-) TX. Zelfde les als bij de pomp-link: verwissel-risico is
reëel — vandaar de zend-gate in de firmware.

> **Pin 16 (ketel-relais)** is een interessante extra: mogelijk kan de gasketel-bijstook
> in fase 3 ook met één digitale uitgang (via de relais-driver op het bord) i.p.v. of
> naast het `0xF0`-frame. Eerst verifiëren wat het relais precies schakelt (maakcontact
> ketelvraag?) en of de ketel dan werkelijk start — meten vóór gebruiken.

## 5. Softwareontwerp

Nieuwe module `ot.h`/`ot.cpp`, zelfde principes als `adam.cpp` (niets blokkeert, geen
publishes in tijdkritieke paden):

1. **Driver:** minimale SC16IS750-laag over `Wire` (init 9600 8N1, `ot_available()`,
   `ot_read()`, `ot_write(buf,12)`). I²C-transacties zijn kort (~0,5 ms) en de
   LCD-updates delen de bus al.
2. **Framer:** 12 bytes verzamelen, checksum controleren; bij fout buffer leegtrekken
   en opnieuw synchroniseren (zoals `clearSerialBuffer` in jgc.ino).
3. **RX-verwerking:** byte 0 → `ot_sp` (clampen zoals jgc.ino: >max → max, ≤15 → 0,
   10 = koelvraag — verifiëren in fase 0).
4. **TX-scheduler:** antwoord ~200 ms na ontvangst én keepalive ≤ 4,2 s; frame opbouwen
   uit template + categorie-logica (§2) + `t_supply`/`t_return`/`t_outside` + checksum.
5. **Bronintegratie:** `t_water_gewenst`-bron wordt een nette keuze: `mqtt` / `adam` /
   `ot` (uitbreiding van het bestaande `adam_bron`-mechanisme, EEPROM). Bij OT-bron
   vervalt de Adam-poll voor het setpoint; de zones/multi-modus kan Adam blijven doen.
6. **MQTT/HA:** `chofu/ot/sp`, `chofu/ot/status`, `chofu/ot/fout` + discovery; web-tab
   "OT-link" naar analogie van de Adam-tab.
7. **Veiligheid:** bij link-uitval terugvallen op de vorige bron + alert; de bestaande
   beveiligingen (SUPPLY_MIN, vorst, KOELING_MIN_BUITEN) blijven leidend. Schrijven
   naar IC1 pas activeren na fase 0/1 (zie onder) — IC1 stuurt de **gasketel** aan,
   dus verkeerde frames kunnen de ketel laten stoken.

## 5b. Bescherming van IC1 (LET OP: bij JGC is de OT-chip gesneuveld!)

JHN meldt dat IC1 tijdens zijn experimenten kapot is gegaan. IC1 is niet los
verkrijgbaar — sneuvelt hij, dan is de controlbox (OpenTherm naar Anna én ketel)
onbruikbaar. Waarschijnlijke doodsoorzaken en de maatregelen daartegen:

| Risico | Maatregel |
|---|---|
| **Driver-conflict**: onze TX op de verkéérde pad (IC1's TX-uitgang) → twee push-pull uitgangen vechten → uitgangstrap kapot. Dit is exact de pad-verwisseling die we bij de pomp-link ook hadden — daar redde de 1 kΩ ons. | **Altijd ≥1 kΩ serie in élke draad naar IC1** (begrenst conflictstroom tot <5 mA). Eerste contact zelfs via 10 kΩ, pas verlagen na verificatie. Plus software-gate: zie onder. |
| **Software-gate tegen verwisseling**: | firmware zendt **pas nadat ≥3 geldige frames ontvangen zijn** — kloppen RX-frames, dan zit onze RX op IC1-TX en dus onze TX op de juiste pad. Verkeerd om = we ontvangen niets = we zenden nooit. |
| **Terugvoeding**: Arduino aan terwijl controlbox uit → 5 V via TX-pin in een spanningsloze IC1 → latch-up. | TX **open-collector** uitvoeren (transistor of diode + pull-up naar de 5 V van de contrólbox zelf): wij kunnen alleen naar GND trekken, nooit spanning índuwen. De pull-up sterft mee met de voeding van IC1. |
| **Massa/net-potentiaal**: laptop met USB-TTL aan een controlbox waarvan de voeding mogelijk niet geïsoleerd is → grote vereffeningsstroom. | Vóór fase 0: verifieer dat de controlbox-elektronica SELV/geïsoleerd is (meet AC-spanning pads↔aarde). Gebruik een laptop op accu of een USB-isolator (ADUM3160, ~€10). |
| **Volledige zekerheid** | **Opto-isolatie op beide richtingen** (H11L1/6N137; PC817 is op 9600 baud randgeval). Galvanisch gescheiden = geen conflict-, terugvoedings- of massaprobleem mogelijk. Meerprijs ~€3. **Aanbevolen voor de definitieve inbouw.** |
| **Sondes/uitglijders**: kortsluiting tussen naburige pads bij meten op een draaiend bord. | Meten alleen spanningsloos aansluiten (haakjes/clips i.p.v. losse probes); daarna pas inschakelen. Foto's vooraf. |
| **ESD** | Polsband of eerst GND aanraken; niet solderen op een gevoed bord. |

**Kortom:** fase 0 alléén luisteren (RX-draad + 10 kΩ, TX niet aangesloten), definitieve
inbouw met opto-isolatie en de zend-pas-na-geldige-ontvangst-gate in de firmware.

## 6. Gefaseerde aanpak

| Fase | Wat | Risico |
|---|---|---|
| **0. Passief meeluisteren** | USB-TTL-adapter + laptop op pad "IC1-TX" + GND (9600 8N1): verifiëren dat IC1 nog 12-byte frames stuurt, checksum klopt, byte 0 = Anna-setpoint volgt, en wat Anna-koelen (10 °C?) op deze link doet. Geen firmware nodig. | Geen |
| **1. Read-only in firmware** | Eerst proberen met SoftwareSerial RX-only op D8 (via ≥1 kΩ; TX-draad fysiek niet aangesloten) — branch `feature/ot-link`. Module luistert alleen en publiceert naar MQTT; vergelijken met Adam-API-waarden. Bij WiFi/JGC-degradatie → SC16IS750 + level shifter. | Laag |
| **2. Schrijven: status terugmelden** | TX-pad aansluiten; template + temperaturen + keepalive sturen → controlbox uit storing. Ketelgedrag observeren. | Middel |
| **3. Bronkeuze + hybride** | `t_water_gewenst` uit OT-link; `0xF0`-logica voor gasketel-bijstook bij extreme kou; koelvraag. | Middel |

## 7. Open vragen

1. Exacte padposities van de IC1-link op het voetje (fase 0 beantwoordt dit).
2. Betekenis van bytes 1–3 en 10 in beide richtingen (template lijkt te volstaan — JHN
   draaide er maanden mee).
3. Hoe tapwater (SWW) over deze link loopt — relevant voor de bestaande SWW-modus.
4. Stuurt IC1 nog frames terwijl de box in storing staat? (Waarschijnlijk wel —
   anders had jgc.ino nooit uit de storing kunnen komen; fase 0 bevestigt dit.)
5. Koelvraag-conventie op deze link (Anna 10 °C zoals bij de Adam-API?).
