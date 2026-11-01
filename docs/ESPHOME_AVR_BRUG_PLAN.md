# Plan: AVR-brug in IC2 en (later) overstap naar ESPHome

Status: **analyse, nog niets gebouwd** (november 2026).
Bron: [Aapjedotinfo/atlantic-aurea-esphome](https://github.com/Aapjedotinfo/atlantic-aurea-esphome)
(bekeken op commit `3691cd5`, 27-10-2026).

## 1. Samenvatting

Het ESPHome-project vervangt **beide** chips in de controlbox:

| Voet | Origineel | Bij hen | Bij ons nu |
|---|---|---|---|
| IC1 | Haddon M1.1 (pomp-chip) | ESP32 MiniKit met ESPHome | UNO R4 WiFi via draden (`Serial1`, 666 baud) |
| IC2 | Haddon T1.1 (OpenTherm-brug) | **eigen ATmega328P** met `avr-bridge` | origineel T1.1, wij luisteren alleen mee (fase 1, RX-only) |

De IC2-kant is het interessante deel: de 328P-brug zit tussen thermostaat (bij ons de
Adam) en CV-ketel, geeft OpenTherm door en laat de controller via een 9600-baud-link
**de ketel blokkeren**, **de WP-temperaturen aan de thermostaat tonen** en **het
storingslampje bedienen**. Dat lost op wat wij nu met de workaround "ketel op alleen
tapwater" omzeilen, en het origineel T1.1 hoeft nooit meer aan een experiment te
worden blootgesteld (hij gaat op de plank).

**Aanbevolen volgorde:**

1. **Scenario A** — huidige firmware op de UNO R4 houden, T1.1 vervangen door de 328P met
   `avr-bridge.hex`, en `ot.cpp` herschrijven naar hun linkprotocol (§4–§6).
2. **Scenario B** — later eventueel IC1 ook naar ESP32/ESPHome, met onze ff_water-modus
   geport (§7). De brug uit scenario A blijft dan gewoon zitten; die stap raakt alleen IC1.

Scenario A is los bruikbaar: zelfs als B nooit gebeurt, is het een verbetering.

## 2. Functievergelijking

| Functie | Onze firmware | ESPHome-project |
|---|---|---|
| Pompprotocol 0x19/0x91 | ✅ JGC multi-frame | ✅ zelfde basis (WackoH/JGC), plus 40 ms lijnstilte-check |
| Extra pompwaarden (Hz, persgas, stroom, …) | ✅ sinds `1ad8191` | ✅ |
| Water-modus (regelen op aanvoersetpoint) | ✅ `water` (PID), `ff_water` (feedforward) | ≈ modus `auto` (eigen standlogica: ΔT-rem, middeling, vooruitkijken, stopvertraging) |
| Setpoint van de thermostaat | Adam-API (`intended_boiler_temperature`), of OT-link fase 1 | **OpenTherm Data-ID 1** via de brug — geen API nodig |
| Kamer-/stooklijnregeling | `auto`, `ff_auto` | `stooklijn`, `kamertemperatuur` |
| Koelen | ✅, incl. Adam-koelvraag (SP = 10) en FF-koelregelaar | ✅ handmatige schakelaar, vast setpoint |
| Multi-zone (zone met grootste vraag) | ✅ via Adam-API | ❌ |
| SWW-driewegklep (relais + overlay-regelaar) | ✅ | ❌ |
| Ketel blokkeren / bijspringen | ❌ (ketel op alleen-tapwater) | ✅ rem + bijstookbeleid in drie trappen |
| WP-temperaturen naar de thermostaat | ❌ | ✅ (ID 25/28-substitutie) |
| Storingslampje | brandt rood | uit / bedienbaar |
| Noodbrug als de controller wegvalt | ❌ | ✅ AVR trekt na 60 s K1/K2 aan: thermostaat direct aan de ketel |
| HA-integratie | MQTT + discovery | ESPHome native API |
| LCD / LED-matrix | ✅ | webinterface |

## 3. Tegenstrijdigheden in de protocolkennis (eerst checken)

Het ESPHome-project heeft op **hun** units twee dingen anders gezien dan
`docs/extra-comm-info.txt` en onze firmware:

1. **91-1 byte 4 (wij: defrost).** Volgens hen is dit **0x01 zodra de unit draait** en
   géén ontdooibyte; zij leiden ontdooien af uit ΔT < −1 °C (30 s vertraagd, niet in
   koelbedrijf). Onze `chofu/defrost` zou dan bij elke run aan staan.
2. **91-3 byte 13 (wij: circulatiepomp).** Bij hen **altijd 24**, ook als de unit draait.
   Onze nieuwe `chofu/circ_pomp` blijft dan altijd 0.

**Check:** een paar dagen HA-historie van `chofu/defrost` naast `chofu/delta_t` en
`chofu/aan`, en `chofu/circ_pomp` naast `chofu/aan`. `chofu/diag` (`pomp_raw`,
`status2` bit 0) helpt daarbij. Pas daarna de firmware/docs aan.

## 4. Scenario A: UNO R4 + 328P-brug in IC2

### 4.1 Wat er verandert

| | Nu (T1.1 origineel) | Straks (328P + `avr-bridge`) |
|---|---|---|
| Link IC1↔IC2 | 9600 8N1, 12-byte frames, kop 0xF0, som-checksum | 9600 8N1, **ander protocol** (§5), som-checksum |
| Wat wij ontvangen | byte 0 = setpoint thermostaat, rest grotendeels onbekend | status (24 B, elke 2 s) + **elk OT-frame rauw** (7 B) |
| Wat wij sturen | niets (fase 1) | commando (16 B) elke ~2 s; optioneel injecties |
| Zonder onze berichten | T1.1 in storing, rode led, stuurt ketel aan | na 60 s **noodbrug** (K1/K2): thermostaat direct op de ketel; lampje knippert |
| Risico bij fout zenden | T1.1 is onvervangbaar | 328P kost een paar euro; opnieuw branden |

**Belangrijk:** zolang wij niets sturen, gedraagt de brug zich ongeveer zoals de huidige
situatie (thermostaat stuurt de ketel). Inbouwen zonder firmwarewijziging is dus veilig,
maar levert nog niets op.

### 4.2 Branden van de 328P

Gebruik de kant-en-klare hex uit hun repo: `firmware/avr-bridge/avr-bridge.hex`
(geen Arduino-toolchain nodig). Fuses:

| Fuse | Waarde | Betekenis |
|---|---|---|
| lfuse | `0xFF` | extern kristal (8 MHz zit al op de print onder IC2) |
| hfuse | `0xD9` | geen bootloader, reset naar adres 0 |
| efuse | `0xFD` | brown-out 2,7 V |

Niet "Bootloader branden" uit de IDE gebruiken (zet `hfuse = 0xDA` → start in leeg
bootloadergebied). Laat RSTDISBL/DWEN met rust.

**Met een Arduino als ISP** (route volgens hen *nog door niemand beproefd*; met een TL866
wel bewezen):

- ArduinoISP-sketch in een Uno; bedrading D13→pin 19 (SCK), D12→18 (MISO), D11→17
  (MOSI), D10→1 (RESET), 5V→7+20, GND→8+22; 10 µF tussen RESET en GND van de Uno
  (pas ná het uploaden van de sketch).
- **Kip-en-ei met de klok:** na `lfuse = 0xFF` heeft de chip een kristal nodig. Dus
  eerst flashen (verse chip draait op 1 MHz intern, `-B 125`), **daarna** de fuses:

```bash
avrdude -c avrisp -P /dev/cu.usbmodemXXXX -b 19200 -p m328p -B 125 -U flash:w:avr-bridge.hex:i
```

```bash
avrdude -c avrisp -P /dev/cu.usbmodemXXXX -b 19200 -p m328p -U lfuse:w:0xFF:m -U hfuse:w:0xD9:m -U efuse:w:0xFD:m
```

- Of de ArduinoISP-sketch op de **UNO R4** werkt is niet bekend; een klassieke Uno of
  Nano is de veilige keuze. Voor een bureautest zonder kristal: `lfuse = 0xE2`
  (intern 8 MHz), zelfde hex.
- Neem de hex mee in een eigen map (bijv. `bridge/avr-bridge.hex` met een README die de
  bron-commit noemt), zodat we niet afhankelijk zijn van hun `main`.

### 4.3 Hardware en bedrading

1. Controlbox spanningsloos. **T1.1 uit IC2 halen en bewaren** (antistatisch, label erop).
2. 328P in IC2, pin 1 dezelfde kant als het origineel.
3. Dipswitch 2 controleren: **ON = OpenTherm-bedrijf** (volgens hun analyse; socket-pin 25).
4. Link naar de UNO R4 (de sporen IC1↔IC2 liggen al op de print):

```
IC1-voet (leeg)                      Arduino UNO R4 (5 V)
pin 2  (AVR-TX → wij) ──[10 kΩ]────── D8  OT_RX_PIN   (bestaat al, fase 1)
pin 3  (wij → AVR-RX) ──[1 kΩ]─────── D9  OT_TX_PIN   (nieuw)
pin 8  GND ────────────────────────── GND (al gedeeld)
```

   De UNO R4 is 5 V, net als de AVR: geen spanningsdeler nodig (die heeft de ESP32 wel).
   De 1 kΩ in de TX-lijn is alleen stroombegrenzing bij een bedradingsfout.
5. Workaround "ketel op alleen tapwater" pas terugdraaien als fase A4 (rem) werkt.

### 4.4 Het grote risico: de tweede UART op de UNO R4

`Serial1` (D0/D1) is bezet door de pomp. De OT-link draait nu op **SoftwareSerial**
(renesas-core, GPT-timers + DMA). In `ervaringen.md` staat dat SoftwareSerial eerder de
WiFi-coprocessor liet crashen; fase 1 was juist bedoeld om dat met de nieuwe core te
testen. De brug stuurt meer verkeer dan T1.1 (24 B per 2 s plus 7 B per OT-frame, enkele
per seconde) en we moeten nu óók zenden.

| Optie | Oordeel |
|---|---|
| SoftwareSerial (RX + TX) | Eerst proberen als fase 1 stabiel bleek. Meten: blokkeert `write()` van 16 B (~17 ms)? Zo ja, alleen zenden direct ná een eigen pomp-TX, nooit in het 99 ms-venster vóór een pomp-TX. |
| SC16IS750 I²C-UART | Fallback: hardware-FIFO, op de I²C-bus naast het LCD. 3,3 V-chip → level shifter. |
| Overstappen naar ESP32 | 3 hardware-UARTs; zie scenario B. Firmware compileert al voor ESP32. |

**Open vraag:** hoe stabiel liep fase 1 (RX-only) op de R4? Dat bepaalt of we hier met
SoftwareSerial kunnen beginnen.

## 5. Het linkprotocol van `avr-bridge`

Alle berichten: vaste lengte, herkend aan het startbyte, laatste byte =
**8-bits som van alle voorgaande bytes**. Temperaturen als **getekende tienden °C,
big-endian**. (Het commentaarblok in hun `aurea_link.h` noemt nog 17/12 bytes; de code
gebruikt 24/16 — de tabellen hieronder volgen de code.)

### 5.1 AVR → controller: status `0xF0`, 24 bytes, elke 2 s

| Byte | Inhoud |
|---|---|
| 0 | `0xF0` |
| 1 | masterstatus thermostaat (OT ID0 hoog): bit0 CV, bit1 tapwater, **bit2 koelen** |
| 2 | slavestatus ketel (ID0 laag): bit0 storing, bit1 CV actief, bit2 tapwater actief, bit3 vlam |
| 3 | gevraagd aanvoersetpoint (ID1, hele °C, **vóór** de rem) |
| 4–5 | kamertemperatuur (ID24) |
| 6–7 | kamersetpoint (ID16) |
| 8–9 | keteltemperatuur (ID25 van de ketel) |
| 10 | modulatie % (ID17) |
| 11 | storingscode (ID5) |
| 12 | bit0 thermostaat leeft, bit1 ketel leeft, bit2 rem actief, bit3 beleid geldig; bits 4–7 volgnummer |
| 13 / 14 | ruwe flanken thermostaat- / ketelkant |
| 15 | lage nibble complete frames, hoge nibble pariteitsfouten |
| 16 | lijnstanden; bits 4–6 = vlaggen zoals de AVR ze van ons **ontving** (bewijs dat TX werkt) |
| 17 / 18 | RX-fouten thermostaat- / ketelkant |
| 19 | I/O: bit0 voorschakelaar, bit1 relais, bit3 vraaguitgang (pin 28) |
| 20 | reserve-ingangen |
| 21 | lage nibble resetoorzaak, hoge nibble bootteller |
| 22 | ISR-overruns (hoort 0) |
| 23 | som |

### 5.2 AVR → controller: OT-frame `0xF2`, 7 bytes, bij elk frame

| Byte | Inhoud |
|---|---|
| 0 | `0xF2` |
| 1 | richting: 0 thermostaat→, 1 ketel→, 2 →ketel (na substitutie), 3 →thermostaat |
| 2–5 | OT-frame, MSB eerst (bit 30–28 type, 23–16 Data-ID, 15–0 waarde, f8.8) |
| 6 | som |

Antwoorden met type ≠ 4/5 (Read-Ack/Write-Ack) negeren: dat is "ken ik niet".

### 5.3 Controller → AVR: commando `0xF1`, 16 bytes, elke ~2 s

| Byte | Inhoud |
|---|---|
| 0 | `0xF1` |
| 1 | bit0 **rem**, bit1 **temp-substitutie**, bit2/3 leesrichting thermostaat/ketel omkeren, bit4 relais, bit5 lampje omkeren, bit6 vlam (zelfstandig), bit7 forceer zelfstandig |
| 2 | bit0/1 zendrichting omkeren, bit2 kanalen wisselen, bit3 relais volgt schakelaar, bit4 K3-vraag toestaan, bit5 **CV-vlag onderdrukken** |
| 3 | rem-setpoint (hele °C, typisch 10) |
| 4–5 | WP-aanvoer voor de thermostaat |
| 6–7 | WP-retour voor de thermostaat |
| 8–9 | override kamersetpoint (0 = geen) |
| 10 | lampje: 0 auto, 1 uit, 2 aan, 3 traag, 4 snel knipperen |
| 11 | modulatie in zelfstandige stand |
| 12 | testuitgangen |
| 13 | rechtstreeks ketelsetpoint zonder thermostaat (0 = uit) |
| 14 | max. ketelmodulatie (0 = niet ingrijpen) |
| 15 | som |

Gedrag in de AVR (niet door ons te omzeilen, en dat is goed):

- **60 s geen geldig commando** → beleid vervalt: rem uit, substitutie uit, overrides weg,
  noodbrug-relais aan.
- **Rem minimaal 5 min aan of uit** (anti-pendel), ongeacht wat wij sturen.
- Stuur bij een eerste test alle omkeer-/wisselbits **0** (de defaults die bij hen werken).

### 5.4 Controller → AVR: injectie `0xF3`, 7 bytes (optioneel)

Modus 0 = frame naar de ketel sturen, 1 = antwoord aan de thermostaat vervangen (max 8),
2 = vervanging opheffen. Bytes 2–5 frame, 6 som. Niet nodig voor de eerste fases.

## 6. Firmwarewerk voor scenario A

### 6.1 Module

`ot.cpp` vervangen door een brugmodule (zelfde naam/API houden waar het kan:
`ot_config`, `ot_poll`, `ot_geldig`, `ot_sp`):

- **RX-parser** met twee lengtes (0xF0 → 24, 0xF2 → 7), resync op startbyte, somcheck;
  geen MQTT-publish in het RX-pad (zelfde regel als bij de pomp).
- **OT-tabel alleen voor de ID's die we gebruiken** (0, 1, 5, 7, 14, 16, 17, 24, 25, 28):
  hun ESP32-tabel van 4 × 256 ID's (~6 kB) past niet in de R4 (nu ~18,8 kB vrij).
- **Commando** elke 2 s (ruim onder de 60 s-timeout); zendmoment kiezen buiten het
  pomp-TX-venster (§4.4).
- Volgnummer-gaten en somfouten tellen (zelfde stijl als de `JGC (30s)`-samenvatting).

### 6.2 Regelaar-koppeling

| Wat | Bron straks | Opmerking |
|---|---|---|
| `t_water_gewenst` (water/ff_water) | status byte 3 / ID1 van de thermostaat | Vervangt de Adam-API als waterbron. **Eerst valideren**: ID1 naast `adam/sp` loggen. |
| Koelvraag | masterstatus bit 2 (en/of ID7) | Meten wat de Adam via OT stuurt bij koelvraag; is het gewoon ID1 = 10, dan blijft onze huidige SP = 10-conventie gelden. |
| Kamertemperatuur / -setpoint | ID24 / ID16 | Bij Adam is dit vermoedelijk de hoofdzone; multi-zone blijft via de Adam-API. |
| Tapwatervraag | masterstatus bit 1 / slavestatus bit 2 | Alleen informatief; SWW-klep blijft zoals hij is. |

### 6.3 Ketelbeleid (eenvoudig beginnen)

1. **Substitutie aan** zodra de pompcommunicatie goed is: thermostaat ziet WP-aanvoer/-retour
   (bytes 4–7 = `t_supply`, `t_return`).
2. **Rem aan** (setpoint 10 °C) zolang: pomp-link ok, modus ≠ handmatig, en buiten ≥ een
   instelbare **bijstookgrens** (bijv. `bijstook_buiten`, default 0 °C).
3. **Rem los** als de pomp-link wegvalt, of als het kouder is dan de grens en de WP
   aantoonbaar tekortschiet (aanvoer > X °C onder setpoint gedurende Y min op max-stand).
   Hun drietrapsbeleid (rust / bijspringen / los) is de referentie voor een latere uitbreiding.
4. **Lampje** op auto (0): de AVR toont zelf of het beleid geldig is.

Nieuwe instellingen (MQTT + EEPROM, magic ophogen): `brug_rem`, `bijstook_buiten`,
`brug_subst`. Nieuwe state-topics: `chofu/ot/status` (bestaat), `chofu/ot/sp`,
`chofu/ot/kamer`, `chofu/ot/koelvraag`, `chofu/ot/ketel_vlam`, `chofu/ot/rem`,
`chofu/ot/link` (missers/somfouten), plus HA-discovery.

### 6.4 Fasering met testcriteria

| Fase | Wat | Klaar als |
|---|---|---|
| **A0** | 328P branden, T1.1 eruit, 328P erin; firmware ongewijzigd (TX-draad los) | Thermostaat stuurt ketel als voorheen (noodbrug); lampje knippert; pomp draait normaal. |
| **A1** | RX-parser voor 0xF0/0xF2, alleen publiceren | Status elke 2 s, geen somfouten, `JGC (30s)` ongewijzigd, geen reboots (≥ 1 dag). **ID1 = `adam/sp`?** **Wat gebeurt er bij koelvraag?** |
| **A2** | TX: commando met alles uit (keepalive) | Status byte 12 bit 3 = 1 (beleid geldig), byte 16 bits 4–6 = wat we stuurden; relais valt af, thermostaat ↔ ketel via de brug. |
| **A3** | Substitutie aan | Adam toont WP-temperaturen als keteltemperatuur. |
| **A4** | Rem aan (eenvoudig beleid §6.3) | Ketel blijft koud bij WP-bedrijf; tapwater blijft werken; workaround "alleen tapwater" kan terug. |
| **A5** | Waterbron Adam-API → OT (optioneel, per instelling) | ff_water draait op ID1 even goed als op de API. |

Afbreken/terug: TX-draad los → binnen 60 s noodbrug. Volledig terug: T1.1 terugplaatsen.

## 7. Scenario B: later naar ESPHome (ESP32 in IC1)

Hun `chofu_wp`-component heeft modi `auto` (vast/thermostaat-setpoint), `stooklijn`,
`kamertemperatuur`, `handmatig`. Wat er voor ons bij moet, in een eigen fork:

| Onderdeel | Hoe | Omvang |
|---|---|---|
| **ff_water** | Nieuwe modus `MODE_FF_WATER` in `chofu_wp.cpp`: doel = thermostaat-ID1 (zoals `auto`), maar de **standkeuze** komt uit onze `pas_ff_aan`/`pas_ff_koel_aan` (UA-leren, coast, deadband ±2 °C, min. stand 1 bij vraag, zachte start, koel-softstart). Geleerde UA in ESPHome `globals` met `restore_value`. | ~350 regels, grotendeels 1-op-1 |
| `water` (PID) | Niet nodig: hun `auto` vervult die rol. | — |
| Adam-koelvraag | YAML-lambda: ID1 = 10 of masterstatus-koelbit → `set_cooling(true)` + koel-setpoint. | klein |
| Multi-zone | HA Plugwise-integratie + automation die de zone met de grootste vraag naar een `number` schrijft. | klein (HA-kant) |
| SWW-klep | `switch: gpio` op een vrije voetpen + overlay (vast setpoint, eigen max-stand) in het component. | klein |
| MQTT → ESPHome | HA-entiteiten wijzigen: dashboards en automations omzetten. | handwerk |

Hardware: ESP32 MiniKit op een adapter in de IC1-voet (hun schema
`docs/schema/adapter-ic1-schema.pdf`), pomp-RX via 4k7 + 10k-deler (3,3 V), brug-RX via
dezelfde deler. Begin met hun **minimale opzet** (`aurea-wp-minimaal.yaml.example`) als
de brug uit scenario A nog niet stabiel is; anders direct de volledige.

Overweging: ff_water als PR bij Aapjedotinfo aanbieden, zodat we niet op een eigen fork
blijven hangen.

## 8. Open vragen

1. Hoe stabiel liep fase 1 (SoftwareSerial RX-only) op de UNO R4? (§4.4)
2. Is `ID1` van de Adam gelijk aan `intended_boiler_temperature` uit de API?
3. Hoe meldt de Adam koelvraag via OpenTherm: koelbit, ID7, of ID1 = 10?
4. Klopt 91-1 byte 4 als defrost en 91-3 byte 13 als circulatiepomp op onze unit? (§3)
5. Werkt ArduinoISP op de UNO R4, of is een klassieke Uno/Nano nodig?
