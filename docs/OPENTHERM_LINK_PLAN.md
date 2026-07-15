# Analyse: OpenTherm-link van de controlbox lezen én schrijven

Status: **analyse/voorstel** — nog niet geïmplementeerd.
Bron: de inter-MCU-code in `JGC/jgc.ino` (Mega 2560, 3 UARTs) van JHN.

## 1. Wat is "de OpenTherm-communicatie" hier precies?

**Niet** de ruwe OpenTherm-bus (17V stroomlus, Manchester-encoding). In de Aurea-controlbox
zitten twee MCU's:

```
Anna/Smile ──OpenTherm──┐
Gasketel  ──OpenTherm──┤  IC1 (OT-MCU)  ←──9600 baud TTL──→  IC2 (HP-MCU, verwijderd!)
                        └────────────────────────────────────────│
                                                          666 baud → warmtepomp
```

- **IC1** doet al het echte OpenTherm-werk (Anna, ketel) en blijft gewoon op het bord zitten.
- **IC2** is de chip die wij verwijderd hebben. Onze Arduino bedient nu wél zijn
  pomp-kant (666 baud), maar **niet** zijn IC1-kant (9600 baud) — daardoor staat de
  controlbox in storing (rode knop) en start hij de gasketel bij warmtevraag.

Het aan te sluiten signaal is dus een gewone **TTL-UART op 9600 baud, 8N1**, op de
vrijgekomen pads van het IC-voetje ("pad 1" = RX van IC1, "pad 2" = TX naar IC1 in
JHN's notatie; in de praktijk één full-duplex UART).

## 2. Protocol (uit jgc.ino, deels reverse-engineered)

**Framing:** vaste **12 bytes**, checksum = som van bytes 0–10 mod 256 in byte 11.

**IC1 → Arduino** (`dataArrayOT_IC2_IC1`):

| Byte | Betekenis |
|---|---|
| 0 | **Gewenst aanvoersetpoint van Anna** (°C, integer) — de OpenTherm intended boiler temperature |
| 1–10 | Onbekend (niet gebruikt door jgc.ino) |
| 11 | Checksum |

**Arduino → IC1** (`dataArrayOT_IC1_IC2`, template `{32,20,154,0,0,0,0,32,29,7,0,18}`):

| Byte | Betekenis |
|---|---|
| 0 | Setpoint-echo, óf `0xF0` = "CV moet aan" (te koud voor WP) |
| 4,5,6,10 | Op 0 zetten → dwingt IC1 om de WP-temperaturen naar OpenTherm door te geven |
| 7 | `round(T_supply)` |
| 8 | `round(T_return)` |
| 9 | `round(T_outside)`; bij buiten < 4 °C vast `0x04` (voorkomt CV-start) |
| 1–3 | Onbekend — templatewaarden (20, 154, 0) overnemen |
| 11 | Checksum |

**Timing:** antwoord ±200 ms na een ontvangen frame; **maximaal 4,2 s** tussen
zendbeurten (keepalive — anders beschouwt IC1 de link als dood → storing).
Ruim onkritisch vergeleken met de pomp-link (99 ms).

**Hybride logica in jgc.ino** (TemperatureCategory):

| Situatie | Actie richting IC1 |
|---|---|
| Buiten < −15 °C | byte 0 = `0xF0` → gasketel neemt over |
| Buiten < 4 °C, WP draait | setpoint-echo + byte 9 = 4 → CV-start geblokkeerd |
| WP uit / normaal | setpoint-echo + echte temperaturen |

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

> ⚠️ De exacte padnummers van pad 1/pad 2 op het 28-pins voetje moeten geverifieerd
> worden (Tweakers-thread/foto's, of meten: de pad die in rust ~5 V voert en elke
> ~1–4 s activiteit toont is IC1-TX). Zelfde les als bij de pomp-link: labels kunnen
> vanuit de verwijderde chip gedacht zijn.

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
