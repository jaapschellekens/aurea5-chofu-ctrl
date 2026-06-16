# Hardware Bekabeling - ChofuCtrl

Complete bekabeling schemas en aansluitingen.

---

## Overzicht

**3 Hoofdonderdelen + optionele bediening:**
1. Arduino UNO R4 WiFi
2. LCD 16x2 I2C Display
3. Warmtepomp Protocol Interface
4. *(Optioneel)* Drukknopjes voor handmatige standbediening

---

## LCD Display Aansluiting

### Schema

```
┌──────────────────┐
│  LCD 16x2 I2C    │
│  ┌────────────┐  │
│  │ GND VCC    │  │
│  │ SDA SCL    │  │
│  └─┬──┬──┬──┬─┘  │
└────┼──┼──┼──┼────┘
     │  │  │  │
     │  │  │  └──────────┐
     │  │  └──────────┐  │
     │  └──────────┐  │  │
     └──────────┐  │  │  │
                │  │  │  │
   ┌────────────┼──┼──┼──┼────────┐
   │ Arduino    │  │  │  │        │
   │ UNO R4     │  │  │  │        │
   │            │  │  │  │        │
   │        GND ●──┘  │  │        │
   │         5V ●─────┘  │        │
   │         A4 ●────────┘ (SDA)  │
   │         A5 ●────────── (SCL) │
   │                              │
   │        ┌──────┐              │
   │        │ USB  │              │
   │        └──────┘              │
   └──────────────────────────────┘
```

### Pin Mapping

| LCD Pin | Arduino Pin | Functie | Wire Kleur (suggestie) |
|---------|-------------|---------|------------------------|
| GND     | GND         | Ground  | Zwart                  |
| VCC     | 5V          | Power   | Rood                   |
| SDA     | A4 (SDA)    | Data    | Geel                   |
| SCL     | A5 (SCL)    | Clock   | Groen                  |

### I2C Adressen

**Standaard:** 0x27  
**Alternatief:** 0x3F

**Check adres met test code:**
```cpp
# include <Wire.h>

void setup() {
  Serial.begin(115200);
  Wire.begin();
  Serial.println("I2C Scanner");
  
  for(byte i = 1; i < 127; i++) {
    Wire.beginTransmission(i);
    if(Wire.endTransmission() == 0) {
      Serial.print("Found: 0x");
      Serial.println(i, HEX);
    }
  }
}

void loop() {}
```

---

## Warmtepomp Protocol Interface

### BELANGRIJKE VEILIGHEID

```
LET OP:
✓ Protocol is 5V (veilig voor Arduino)
✓ Gebruik ALTIJD weerstand op TX pin (D1)
✓ Test eerst op breadboard
✓ Maak foto's voor je begint
✓ Bij twijfel: vraag hulp!
```

---

## Directe Verbinding

> **Let op:** De firmware gebruikt de hardware UART (`Serial1`, pins D0/D1), **niet** SoftwareSerial op pin 2/3. Sluit de controlbox daarom aan op D0 en D1.

> **⚠️ TX/RX-labels op het IC-voetje zijn vanuit de VERWIJDERDE CHIP gezien!**
> De Arduino *vervangt* de chip en moet dus de chip-pads als de chip zelf aansturen:
> Arduino **TX (D1) → pad "TX"** en Arduino **RX (D0) → pad "RX"**.
> Dit is dus *omgekeerd* aan de gebruikelijke conventie (TX→RX) bij twee losse apparaten.
> Verkeerd om aangesloten = TX gaat wel uit ("JGC timeout: geen frame >2s") maar de pomp antwoordt nooit.

### Schema

```
Controlbox (IC-voetje,        Arduino UNO R4
labels v/d verwijderde chip)
┌─────────────┐              ┌──────────────┐
│             │              │              │
│    RX   ●───┼──────────────┼──● D0 (RX1)  │
│         │   │              │              │
│    TX   ●───┼────[1kΩ]─────┼──● D1 (TX1)  │
│         │   │   weerstand  │              │
│   GND   ●───┼──────────────┼──● GND       │
│             │              │              │
└─────────────┘              └──────────────┘
```

### Stappen

1. **Identificeer Controlbox Terminals:**
```
Op controlbox PCB zoek (bij het 28-pins IC-voetje, zie foto verderop):
- TX pad (label "TX")  — was de uitgang van de verwijderde chip richting pomp
- RX pad (label "RX")  — was de ingang van de verwijderde chip vanaf de pomp
- GND pad (label "GND" of "-")
```

2. **Soldeer Weerstand:**
```
1kΩ weerstand in serie met Arduino D1 (TX1)
Dit beschermt controlbox tegen overbelasting
```

3. **Aansluitingen:**
```
Pad "RX"  →  Arduino D0 (RX1)  [Direct]       (pomp → Arduino)
Pad "TX"  →  Arduino D1 (TX1)  [Via 1kΩ!]     (Arduino → pomp)
GND       →  Arduino GND       [Direct]
```

4. **Verificatie:**
```
Goed om:    binnen ~2s 0x91-frames terug van de pomp
Verkeerd om: herhaald "JGC timeout: geen frame >2s, stuur TX" → draai D0/D1 om
```

### Bill of Materials (BOM)

| Item | Aantal | Prijs | Link |
|------|--------|-------|------|
| 1kΩ Weerstand (1/4W) | 1 | €0.10 | - |
| Jumper Wires (F-F) | 3 | €1.00 | - |
| Krimptang connectors | 6 | €0.50 | - |

---

## Foto Referenties

### Controlbox Terminals

![image](pins.avif)

Foto van https://gathering.tweakers.net/forum/view_message/84924782

**Let op:**
- Sommige controlboxen hebben andere labels op de chip.
- Bij twijfel: meet met multimeter

---

## Testing & Verificatie

### Stap 1: Continuïteit Check (Multimeter)

**Voor aansluiten van Arduino:**
```
1. Zet multimeter op continuïteit (♪)
2. Check elke verbinding:
   - Touch probe op controlbox TX
   - Touch probe op Arduino D0 (RX1)
   - Moet piepen (verbinding OK)
3. Herhaal voor alle verbindingen
```

### Stap 2: Voltage Check

**Controlbox uit, Arduino uit:**
```
Multimeter op DC voltage (20V range)

Tussen pad "TX" en GND:
Expected: 0V (geen spanning zonder stroom)

Tussen pad "RX" en GND:
Expected: 0V
```

**Controlbox aan, Arduino uit:**
```
Tussen pad "RX" en GND (lijn vanaf de pomp):
Expected: ~5V in rust (pomp-zender idle high)

Meet ook op Arduino D0 (RX1) (moet zelfde zijn)
```

> **Let op (chip verwijderd):** de pomp antwoordt alleen op geldige polls.
> Zonder pollende Arduino kan de lijn stil zijn — dat is normaal, geen defect.

### Stap 3: Data Verificatie (Serial Monitor)

**Upload test sketch:**
```cpp
void setup() {
  Serial.begin(115200);
  Serial1.begin(666);   // hardware UART op D0/D1
  Serial.println("Protocol Test");
}

void loop() {
  if(Serial1.available()) {
    byte b = Serial1.read();
    Serial.print("RX: 0x");
    Serial.println(b, HEX);
  }
}
```

> **ESP32:** gebruik `Serial1.begin(666, SERIAL_8N1, 16, 17)` en pas pinnen aan voor jouw board.

**Verwacht:**
```
Protocol Test
RX: 0x91
RX: 0x00
RX: 0x01
RX: 0x23
...
```

**Als geen data:**
```
Check:
□ Wires correct aangesloten?
□ Controlbox heeft stroom?
□ Warmtepomp draait?
□ GND gemeenschappelijk?
□ Baud rate correct? (666)
```

---

## Troubleshooting

### Geen Data Ontvangen

**Symptomen:**
- Serial Monitor toont niets
- Geen "RX: 0x91" messages

**Oplossingen:**
```
1. Check bekabeling (pad "RX" → Arduino D0, pad "TX" → Arduino D1 via 1kΩ)
   LET OP: labels zijn vanuit de verwijderde chip — Arduino TX op pad "TX"!
2. Verify controlbox heeft stroom
3. Check GND verbonden (gemeenschappelijke ground!)
4. Test met multimeter: ~5V rust op pad "RX"?
5. "JGC timeout: geen frame >2s" in de log = TX werkt maar geen antwoord
   → meestal D0/D1 verkeerd om: wissel de draden
6. Check dat je D0/D1 gebruikt (hardware UART), niet pin 2/3
7. Check protocol: JGC frames ontvangen op chofu/proto/rx? (proto_log aanzetten)
```

### Garbage Data

**Symptomen:**
- Vreemde tekens in Serial Monitor
- Random bytes, geen patroon

**Oplossingen:**
```
1. Check baud rate (moet 666 zijn)
2. EMI interferentie? (gebruik kortere wires)
3. Ground loop? (check GND verbinding)
4. Gebruik shielded cable voor lange afstanden
```

### Arduino Reboot bij Protocol Verbinden

**Symptomen:**
- Arduino reset zodra protocol aangesloten
- Onregelmatige resets

**Oplossingen:**
```
1. Kortsluit ergens? Check alle verbindingen
2. Te veel stroom draw? Gebruik aparte 5V voeding
3. Ground loop? Check GND niet dubbel aangesloten
4. Gebruik optocoupler (galvanische scheiding)
```


---

## Handmatige Bediening — Knoppen (optioneel, niet goed getest)

Twee drukknopjes maken het mogelijk de WP-stand direct in te stellen, zonder WiFi of MQTT. Indrukken zet de modus automatisch op HANDMATIG; MQTT-commando's kunnen daarna nog steeds overschrijven.

### Aanbevolen knoppen

| Situatie | Type | Formaat | Prijs | Tip |
|----------|------|---------|-------|-----|
| Testen / breadboard | Tactile push button | 6×6mm of 12×12mm | €0,10 | Zit in elk Arduino starterskit |
| Permanente montage | Paneel-montage drukknop (NO) | 16mm of 22mm | €1–3 | Roestvrij staal, rood/groen |

**Zoekterm**: `"16mm momentary push button NO"` of `"22mm panel mount momentary"`  
**Verkrijgbaar bij**: Okaphone, Conrad, AliExpress, Farnell

> Kies voor de NO-uitvoering (Normally Open). LED-verlichting in de knop is optioneel — sluit de LED dan gewoon niet aan.

### Bedrading

```
Arduino D5 ●────┤  ▲ BTN UP   ├──── GND     (stand omhoog)
Arduino D6 ●────┤  ▼ BTN DOWN ├──── GND     (stand omlaag)
```

Geen externe weerstand nodig — de firmware activeert de interne pull-up (`INPUT_PULLUP`).

### Pin Mapping

| Functie | Arduino Pin | Knop-aansluiting |
|---------|-------------|------------------|
| Stand omhoog | D5 | NO contact → GND |
| Stand omlaag | D6 | NO contact → GND |

### Schema

```
┌──────────────────────────────────────────┐
│ Arduino UNO R4                           │
│                                          │
│   D5 ●──────────────┐                   │
│                      │  ╔════════╗       │
│                      └──║ BTN UP ║──┐   │
│                         ╚════════╝  │   │
│                                     │   │
│   D6 ●──────────────┐               │   │
│                      │  ╔══════════╗│   │
│                      └──║ BTN DOWN ║│   │
│                         ╚══════════╝│   │
│                                     │   │
│  GND ●──────────────────────────────┘   │
└──────────────────────────────────────────┘
```

### Gedrag

| Actie | Effect |
|-------|--------|
| Kort drukken | Stand ±1, modus → HANDMATIG, LCD en MQTT direct bijgewerkt |
| Ingehouden houden | Auto-repeat elke 200 ms (handig voor snel naar stand 0 of 12) |
| MQTT-commando daarna | Overschrijft de handmatige stand gewoon |

### Bill of Materials — Knoppen

| Item | Aantal | Prijs (schatting) |
|------|--------|-------------------|
| 16mm paneel drukknop NO (rood) | 1 | €1,50 |
| 16mm paneel drukknop NO (groen) | 1 | €1,50 |
| Jumper wires (M-F) | 2 | €0,20 |
| **Totaal** | | **€3,20** |

---

## Het werkt niet?

**Check:**
1. INSTALLATION.md - Stap-voor-stap guide
2. Serial Monitor (115200 baud) - Debug info
3. Multimeter - Voltage/continuity checks
4. GitHub Issues - Community hulp


