# Analyse winter 2026: defrosts, capaciteit en bijstook

Stand: 27 november 2026. Werkdocument om verder te gaan als het kouder wordt.
Opstelling: ESP32 met ESPHome (Aapjedotinfo + eigen aanpassingen in `esphome/`), AVR-brug v1 in IC2,
Plugwise Adam/Anna, OpenTherm-ketel. Ketel en warmtepomp zitten elk op een **eigen tak** van de
hoofdleidingen (parallel), met terugslagkleppen in beide takken en een bypass vlak bij de WP.
Zoneregeling via de Adam; er staat altijd minstens één T22-radiator (220 × 60 cm) open.

## 1. Data en werkwijze

- Export: HA → Geschiedenis → entiteiten kiezen → ⋮ → *Download data* (CSV of Excel).
- Script: [`python/ha_history_analyse.py`](../python/ha_history_analyse.py) (`python3 ha_history_analyse.py history.csv`).
  Leest CSV en xlsx, zet UTC om naar lokale tijd (+1 uur), en geeft defrosts, een uurtabel en
  een schatting van het warmteverlies.
- **Excel-valkuil:** Excel (NL-locale) laat bij float32-waarden de komma vallen
  (`28.7000007629395` wordt `287000007629395`, `0.1` wordt `100000001490116`). Het script
  herstelt dat aan de hand van het float32-patroon en het waardebereik per entiteit.
- Entiteiten die nodig zijn: Aanvoer, Retour, Doel aanvoer, Stand, Vermogen, Buiten (WP-voeler),
  Statusbyte buitenunit (ID3 byte 8), Anna temperatuur, Anna setpoint, Cresta-buitentemperatuur.
  Handig erbij: Temperatuurdata leeftijd, Pomp dubbele start, Bijstookreden, Tekort warmtepomp.
- **Statusbyte ID3 byte 8:** bit 0 = defrost (67/65), bit 1 = warmtevraag van ons (66 ↔ 64),
  bit 6 = 60 min na eerste start.
- **WP-buitenvoeler** leest hier 1,3–1,6 °C hoger dan de Cresta (bij −2,8 Cresta: −1,3 WP).
  Alle grenzen in de software gebruiken de WP-voeler.

## 2. Opgelost: ID2-gaten rond defrosts (23–24 nov)

- Symptoom: na een defrost bleven aanvoer en retour 1 tot 8 minuten bevroren; de regeling
  schakelde blind op (stand 8 bij "20 °C").
- Oorzaak: na een defrost stuurt de unit ~10 min lang ongeveer één keer per rondje een extra
  startbyte (`91 91 02 …`). De parser gooide dan het hele frame erachter weg; vaak was dat ID2.
  Geen storing op de lijn (0 CRC-fouten).
- Oplossing (`esphome/components/chofu_wp`): `91 91` wordt herkend (teller *Pomp dubbele start*),
  na een vals begin worden de bytes opnieuw bekeken, en bij een echt ID2-gat wordt de stand
  vastgehouden (> 15 s), worden temperaturen onbekend (> 120 s) en gaat de unit pas na 15 min uit.
- Bevestigd 24 nov 19:42: temperatuurdata nooit ouder dan 2 s, ook na de defrost.

## 3. Defrosts en kamertemperatuur

| Nacht | Buiten (Cresta) | Kamer t.o.v. setpoint 18,5 | Aanvoer t.o.v. doel per defrostcyclus |
|---|---|---|---|
| 23→24 nov (oude firmware, ID2-gaten) | −0,2 tot −2,6 | zakte tot **−1,2** | 6–10 °C te laag, stand bleef ±3,5 |
| 24→25 nov (nieuwe firmware) | +0,7 tot −0,8 | **−0,1** | 1–4 °C te laag, herstel in 6–11 min |
| 26→27 nov | −1 tot −2,9 | −0,1 tot **−0,3** | uren met defrost 3–5 °C, zonder ±1 °C |

- Defrost: 3–5 min, ongeveer elke 50 min bij −2 à −3 °C en ±85 % luchtvochtigheid.
  Aanvoer zakt dan van ±34 naar 12–13 °C.
- Na een defrost schakelt de regeling snel op (stand 8–10) en schiet soms door tot 38–43 °C
  (doel 35). Bewust zo gelaten; demping kan via *Stap-interval* (180–240 s), *Maximale stand* 8,
  *Opschakelen pas onder stijging* 1,0–1,5, *Middeling aanvoer* laag.
- Conclusie: bij deze temperaturen is een hogere stooklijn niet nodig; de kamer blijft binnen
  0,3 °C. Het eerdere tekort kwam door de ID2-gaten, niet door de defrosts zelf.

## 4. Capaciteitsgrens (26–27 nov)

| Situatie | Stand | Kamer t.o.v. setpoint |
|---|---|---|
| 27 nov 06–07 u, −2,8 °C Cresta | 10, 100 % van het uur | −0,2 tot −0,3 |
| 26 nov 19–21 u, setpoint 19,0 → 19,5, −1 °C | 9,5–9,9 | −0,4 (kamer bleef op 19,1) |

- Het **omslagpunt** (WP net genoeg) ligt rond **−3 °C Cresta** (±−1,5 °C WP-voeler).
- Een sprong van het setpoint met 1 °C haalt de pomp bij vorst niet: beter geleidelijk of eerder beginnen.

## 5. Warmteverlies en benodigd vermogen bij −10 °C

Methode: afgegeven warmte per uur = elektrisch vermogen × COP, met het COP-model uit
`chofu_wp_ff/regelaar.cpp` (`ff_cop`: Carnot × 0,40 op aanvoer en WP-buitentemperatuur).
Uren met zon (10–15 u) weggelaten.

- Script, 26–27 nov: **182 W/K** bruto (28 uur, gem. 20,2 K binnen-buiten, gem. 3,7 kW).
  Dat telt de defrostenergie als warmte mee; netto schat ik **±165–175 W/K** (interne warmte
  van mensen en apparaten zit erin).
- Ter vergelijking: de UNO-firmware heeft `ff_UA_house` = 272,5 W/K (ander model en periode).

| Buiten (Cresta) | Nodig (kamer 18,5–19,5) | WP netto (schatting) | Tekort |
|---|---|---|---|
| −3 °C | ±4,1 kW | ±4,1 kW | ±0 |
| −5 °C | ±4,1–4,5 kW | ±3,7 kW | ±0,5 kW |
| −10 °C | **±4,8–5,2 kW** | **±3,0–3,2 kW** | **±1,6–2,0 kW** |

- WP bij −10 °C: stand 10 ≈ 1,2–1,4 kW elektrisch, COP ±2,5 bij ±40 °C aanvoer → ±3,4 kW,
  minus ±10 % defrost.
- Tekort groeit ±0,25–0,3 kW per graad onder −3 °C.
- **Element:** 2 kW dekt −10 °C net, 3 kW geeft marge (−12 °C, opwarmen na nachtverlaging).
  Eigen groep (3 kW ≈ 13 A). Verbruik: enkele tientallen tot ±100 uur per jaar → ±100–300 kWh.
- **Onzekerheid ±20 %:** COP-model, vermogen incl. circulatiepomp, smal temperatuurbereik.

## 6. Hydraulica: parallel ketel en WP

- Terugslagkleppen in beide takken: geen terugstroming door de WP.
- Risico bij bijspringen: de (sterkere) ketelpomp drukt de WP via zijn bypass rond. De WP-aanvoer
  stijgt dan, de software ziet "geen tekort", zet de ketel terug in rust → **pendelen** met een
  periode van ongeveer de wachttijd.
  Herkenbaar aan: *Bijstookreden* wisselt tussen "bijstook" en "warmtepomp redt het", WP-delta T
  klein en aanvoer + retour stijgen samen zodra de ketel brandt.
- Nog te doen: **test van 30 min samen draaien** (bijstookgrens tijdelijk 15 °C, wachttijd 1 min)
  met export van WP-aanvoer/retour/delta T/stand/frequentie en Keteltemperatuur/modulatie/Vlam.
- Daarom gebouwd: **Omschakelen naar ketel bij kou** (zie §8).

## 7. Buffervat

- Actief watervolume in het slechtste geval (alleen de T22 open): T22 ±13 L + leidingen 10–20 L
  + WP 3–5 L ≈ **25–40 L**. Een defrost (±0,3–0,5 kWh) laat dat ±10–12 °C dalen; dat klopt met de
  gemeten val naar 12–13 °C. De T22 kan bij 35 °C maar ±0,7 kW kwijt → doorschieten na een defrost.
- Vuistregel 10–20 L per kW → 50–100 L. Controleer de **minimale waterinhoud** in de Aurea-handleiding.
- **Serie-buffer (2-pijps) ±50 L in de retour van de WP-tak:** defrostdip ±4 °C i.p.v. 10–12, rustiger
  herstel, geen COP-verlies. Lost het pompprobleem met de ketel niet op.
- **4-pijps buffer:** scheidt ketel en WP hydraulisch, zodat bijspringen werkt; eigen pomp voor de
  radiatorkring nodig, iets COP-verlies door menging.
- Een buffer geeft geen extra vermogen: bij −3 °C en kouder blijft er een tekort.
- Elektrisch element in de buffer kan het tekort uit §5 dekken; aansturing via relais op een vrije
  ESP32-pen, op hetzelfde "tekort"-signaal als het bijstookbeleid (stand vol + aanvoer onder doel).

## 8. Instellingen (stand 27 nov)

Groep Bijstookbeleid (allemaal op de WP-voeler):

| Instelling | Advies | Opmerking |
|---|---|---|
| Omschakelen naar ketel bij kou | aan als je de ketel wilt laten overnemen | eigen toevoeging, standaard uit |
| Alleen ketel onder buitentemperatuur | ±−2,5 (≈ −4 Cresta) | 1 °C hysterese, min. 30 min per stand; ketel krijgt dan de vraag van de Adam |
| Bijstook pas onder buitentemperatuur | ±−2 (als je bijspringen gebruikt) | |
| Wachttijd voor bijstook | ≥ 20–30 min | anders springt de ketel na elke defrost bij |
| Ketelsetpoint bij bijstook | 40–45 °C | |
| Bijstook minimaal boven WP-aanvoer | 8 K | bedoeld voor serie; bij parallel onschuldig |
| Maximale ketelmodulatie | 30–50 % (100 % bij alleen ketel) | |

Groep Instellingen: *Stap-interval* 180–240 s, *Stap-interval omlaag* 30–60 s,
*Vooruitkijktijd aanvoer* 4–5 min, *Middeling aanvoer* 0–60 s, *Opschakelen pas onder stijging* 1,0–1,5.

## 9. Openstaand voor als het kouder wordt

1. **Export bij −5 °C of kouder** (een paar dagen), met de entiteiten uit §1 plus Bijstookreden
   en Tekort warmtepomp. Dan: warmteverlies over een breder temperatuurbereik opnieuw
   bepalen (script), en het tekort bij −10 °C preciezer maken.
2. Hoeveel uur staat de pomp aaneen op de hoogste stand, en hoeveel zakt de kamer?
   Daaruit volgt of 2 of 3 kW element nodig is, en de juiste omschakel-/bijstookgrens.
3. Test van 30 min ketel + WP samen (§6): pendelt het bijspringen?
4. Effect van de demp-instellingen (§3) op doorschieten en kamertemperatuur.
5. Tapwater: de ketel levert nu ook het warme tapwater. Zonder ketel is daar een oplossing voor
   nodig (boilervat met WP-SWW of elektrische boiler).
