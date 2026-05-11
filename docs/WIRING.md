# Hardware Bekabeling - Kromhout Warmtepomp Controller

Complete bekabeling schemas en aansluitingen.

---

## Overzicht

**3 Hoofdonderdelen + optionele bediening:**
1. Arduino UNO R4 WiFi
2. LCD 16x2 I2C Display
3. Warmtepomp Protocol Interface
4. *(Optioneel)* 2 drukknopjes voor handmatige standbediening

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

## Handmatige Bediening — Knoppen

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

## Optie A: Directe Verbinding (Simpel)

**Voor:** Snel te bouwen, weinig onderdelen  
**Tegen:** Geen galvanische scheiding  
**Aanbevolen voor:** Test setup, tijdelijk gebruik

> **Let op:** De firmware gebruikt de hardware UART (`Serial1`, pins D0/D1), **niet** SoftwareSerial op pin 2/3. Sluit de controlbox daarom aan op D0 en D1.

### Schema

```
Controlbox                    Arduino UNO R4
┌─────────────┐              ┌──────────────┐
│             │              │              │
│    TX   ●───┼──────────────┼──● D0 (RX1)  │
│         │   │              │              │
│    RX   ●───┼────[1kΩ]─────┼──● D1 (TX1)  │
│         │   │   weerstand  │              │
│   GND   ●───┼──────────────┼──● GND       │
│             │              │              │
└─────────────┘              └──────────────┘
```

### Stappen

1. **Identificeer Controlbox Terminals:**
```
Op controlbox PCB zoek:
- TX terminal (vaak label "TX" of "SEND")
- RX terminal (vaak label "RX" of "RECV")  
- GND terminal (vaak label "GND" of "-")
```

2. **Soldeer Weerstand:**
```
1kΩ weerstand in serie met Arduino D1 (TX1)
Dit beschermt controlbox tegen overbelasting
```

3. **Aansluitingen:**
```
Controlbox TX  →  Arduino D0 (RX1)  [Direct]
Controlbox RX  →  Arduino D1 (TX1)  [Via 1kΩ weerstand!]
Controlbox GND →  Arduino GND       [Direct]
```

### Bill of Materials (BOM)

| Item | Aantal | Prijs | Link |
|------|--------|-------|------|
| 1kΩ Weerstand (1/4W) | 1 | €0.10 | - |
| Jumper Wires (F-F) | 3 | €1.00 | - |
| Krimptang connectors | 6 | €0.50 | - |

---

## Optie B: Via Optocoupler (Veilig, aanbevolen)

**Voor:** Galvanische scheiding, bescherming  
**Tegen:** Meer onderdelen, complexer  
**Aanbevolen voor:** Permanente installatie, productie

### Schema

```
Controlbox              Optocoupler              Arduino
┌──────────┐            ┌──────────┐            ┌─────────┐
│          │            │ PC817 #1 │            │         │
│   TX  ●──┼──[220Ω]────┼─1    4───┼─[1kΩ]─────┼─● D0    │
│       │  │            │          │       5V   │   (RX1) │
│   GND ●──┼────────────┼─2    3───┼────────────┼─● GND   │
│          │            └──────────┘            │         │
│          │            ┌──────────┐            │         │
│          │            │ PC817 #2 │            │         │
│   RX  ●──┼────────────┼─4    1───┼─[220Ω]────┼─● 5V    │
│       │  │       GND  │          │            │         │
│   GND ●──┼────────────┼─3    2───┼─[1kΩ]─────┼─● D1    │
│          │            └──────────┘            │   (TX1) │
└──────────┘                                    └─────────┘
```

### PC817 Pinout

```
     PC817
   ┌───┴───┐
   │1     4│
   │       │
   │2     3│
   └───────┘
   
Pin 1: Anode (LED)
Pin 2: Cathode (LED)
Pin 3: Emitter (Transistor)
Pin 4: Collector (Transistor)
```

### Breadboard Layout

```
Optocoupler #1 (RX richting - Controlbox → Arduino):
┌─────────────────────────────────┐
│ Controlbox TX                    │
│     ↓                            │
│  [220Ω] → PC817 Pin 1            │
│           PC817 Pin 2 → GND      │
│           PC817 Pin 4 → [1kΩ] → Arduino D0 (RX1)
│           PC817 Pin 3 → GND      │
└─────────────────────────────────┘

Optocoupler #2 (TX richting - Arduino → Controlbox):
┌─────────────────────────────────┐
│ Arduino D1 (TX1)                 │
│     ↓                            │
│  [1kΩ] → PC817 Pin 2             │
│          PC817 Pin 3 → GND       │
│          PC817 Pin 1 → [220Ω] → 5V
│          PC817 Pin 4 → Controlbox RX
└─────────────────────────────────┘
```

### Bill of Materials (BOM)

| Item | Aantal | Prijs | Link |
|------|--------|-------|------|
| PC817 Optocoupler | 2 | €0.40 | - |
| IC voetje 28-pins DIP | 1 | €0.30 | - |
| 220Ω Weerstand (1/4W) | 2 | €0.20 | - |
| 1kΩ Weerstand (1/4W) | 2 | €0.20 | - |
| Breadboard 400 pins | 1 | €2.50 | - |
| Jumper Wires (M-M) | 10 | €1.50 | - |
| **Totaal** | - | **€4.80** | - |

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

Tussen controlbox TX en GND:
Expected: 0V (geen spanning zonder stroom)

Tussen controlbox RX en GND:
Expected: 0V
```

**Controlbox aan, Arduino uit:**
```
Tussen controlbox TX en GND:
Expected: 0-5V (idle state)

Meet ook op Arduino D0 (RX1) (moet zelfde zijn)
```

### Stap 3: Data Verificatie (Serial Monitor)

**Upload test sketch:**
```cpp
void setup() {
  Serial.begin(115200);
  Serial1.begin(9600);   // hardware UART op D0/D1
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

> **ESP32:** gebruik `Serial1.begin(9600, SERIAL_8N1, 16, 17)` en pas pinnen aan voor jouw board.

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
□ Baud rate correct? (9600)
```

---

## Troubleshooting

### Geen Data Ontvangen

**Symptomen:**
- Serial Monitor toont niets
- Geen "RX: 0x91" messages

**Oplossingen:**
```
1. Check bekabeling (controlbox TX → Arduino D0)
2. Verify controlbox heeft stroom
3. Check GND verbonden (gemeenschappelijke ground!)
4. Test met multimeter: spanning op TX pin van controlbox?
5. Wissel RX/TX (verkeerd om aangesloten?)
6. Check dat je D0/D1 gebruikt (hardware UART), niet pin 2/3
```

### Garbage Data

**Symptomen:**
- Vreemde tekens in Serial Monitor
- Random bytes, geen patroon

**Oplossingen:**
```
1. Check baud rate (moet 9600 zijn)
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

## PCB Design (Optioneel)

Voor permanente installatie kan je een custom PCB laten maken:

### Gerber Files

**Coming soon:** PCB design met:
- Arduino header pinnen
- LCD I2C connector
- Optocoupler circuit
- Terminal blocks voor controlbox
- Status LED's
- Power indicator

**Features:**
- Compacte afmetingen (5x7cm)
- Schroefgaten voor montage
- Labeled terminals
- Professional finish

---

## Best Practices

### DO's 
```
✓ Test op breadboard eerst
✓ Gebruik verschillende kleuren per functie
✓ Label alle wires (masking tape + marker)
✓ Maak foto's tijdens installatie
✓ Document je specifieke bekabeling
✓ Gebruik krimptang connectors
✓ Verifieer met multimeter
```

### DON'Ts 
```
✗ Direct solderen zonder test
✗ Te lange wires (EMI gevoelig)
✗ Vergeet GND gemeenschappelijk
✗ Skip weerstand op TX pin
✗ Gebruik slechte jumper wires
✗ Forceer verbindingen
✗ Werk onder spanning
```

---

## Complete BOM (Bill of Materials)

| Item | Aantal | Prijs | Optioneel |
|------|--------|-------|-----------|
| Arduino UNO R4 WiFi | 1 | €30,00 | ❌ |
| LCD 16x2 I2C | 1 | €5,00 | ❌ |
| Jumper Wires F-F | 4 | €1,00 | ❌ |
| 1kΩ Weerstand | 1 | €0,10 | ❌ |
| 16mm paneel drukknop NO | 2 | €3,00 | ✅ |
| PC817 Optocoupler | 2 | €0,40 | ✅ |
| IC voetje 28-pins DIP | 1 | €0,30 | ✅ |
| 220Ω Weerstand | 2 | €0,20 | ✅ |
| Breadboard | 1 | €2,50 | ✅ |
| USB-C kabel | 1 | €5,00 | ❌ |
| 5V Power supply | 1 | €8,00 | ✅ |
| Krimptang connectors | 10 | €1,00 | ✅ |
| **Totaal (basis)** | | **€41,10** | |
| **Totaal (volledig)** | | **€56,20** | |

---

## Hulp Nodig?

**Check:**
1. INSTALLATION.md - Stap-voor-stap guide
2. Serial Monitor (115200 baud) - Debug info
3. Multimeter - Voltage/continuity checks
4. GitHub Issues - Community hulp

**Veiligheid eerst!** Bij twijfel: vraag hulp! 🔧
