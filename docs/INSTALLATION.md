# Installatie Handleiding - ChofuCtrl

Complete stap-voor-stap guide van hardware tot werkend systeem.

---

## Overzicht

**Geschatte tijd:** 2-3 uur  
**Moeilijkheidsgraad:** Gemiddeld  
**Vereiste kennis:** Basis Arduino, simpel solderen (optioneel)

---

## Vereisten

### Hardware
- ✅ Arduino UNO R4 WiFi (~€30) **of** ESP32 board (~€5)
- ✅ LCD 16x2 I2C Display (~€5)
- ✅ 4x Jumper wires (female-female)
- ✅ USB-A naar USB-C kabel
- ✅ Computer (Windows/Mac/Linux)
- ✅ Chofu warmtepomp met controlbox
- ☑️ *(Optioneel)* 2x 16mm paneel drukknop NO — voor handmatige standbediening zonder WiFi

### Software
- ✅ Arduino IDE (gratis download)
- ✅ USB driver (meestal automatisch)

### Netwerk
- ✅ 2.4GHz WiFi netwerk
- ✅ MQTT broker (Mosquitto in Home Assistant)
- ✅ Home Assistant (optioneel maar aanbevolen)

---

## FASE 1: Software Setup

### Stap 1: Arduino IDE Installeren

**Windows:**
```
1. Download: https://www.arduino.cc/en/software
2. Run installer
3. Installeer alle drivers (accepteer alles)
4. Start Arduino IDE
```

**Mac:**
```
1. Download .dmg bestand
2. Sleep naar Applications
3. Open Arduino IDE
4. Sta toe in Security settings (eerste keer)
```

**Linux:**
```bash
sudo apt update
sudo apt install arduino
# Of download van arduino.cc voor nieuwste versie
```

### Stap 2: Board Installeren

**Arduino UNO R4 WiFi:**
```
1. Arduino IDE → Tools → Board → Boards Manager
2. Zoek: "Arduino UNO R4"
3. Installeer: "Arduino UNO R4 Boards" by Arduino
4. Tools → Board → Arduino UNO R4 Boards → Arduino UNO R4 WiFi
```

**ESP32 (alternatief):**
```
1. Arduino IDE → File → Preferences
2. Extra boards URL: https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
3. Tools → Board → Boards Manager → zoek "esp32" → Installeer
4. Tools → Board → ESP32 Arduino → selecteer jouw model
```

### Stap 3: Bibliotheken Installeren

```
Sketch → Include Library → Manage Libraries

Installeer (zoek en klik Install):
✓ ArduinoMqttClient (by Arduino) - Voor MQTT
✓ LiquidCrystal I2C (by Frank de Brabander) - Voor LCD
```

> WiFi is ingebouwd: `WiFiS3` op UNO R4, `WiFi` op ESP32. SoftwareSerial is **niet** nodig — de firmware gebruikt de hardware UART.

---

## FASE 2: Hardware Aansluiten

### Stap 1: LCD Display Verbinden

**Bekabeling:**
```
LCD Pin  →  Arduino Pin
────────     ───────────
VCC      →   5V
GND      →   GND
SDA      →   A4 (SDA)
SCL      →   A5 (SCL)
```

**Foto Guide:**
```
[LCD Module]    [Arduino UNO R4]
┌─────────┐     ┌──────────────┐
│ GND VCC │     │              │
│ SDA SCL │     │   ┌──────┐   │
└─┬─┬─┬─┬─┘     │   │ USB  │   │
  │ │ │ │       │   └──────┘   │
  │ │ │ └───────┼─→ A5 (SCL)   │
  │ │ └─────────┼─→ A4 (SDA)   │
  │ └───────────┼─→ 5V          │
  └─────────────┼─→ GND         │
                └──────────────┘
```

**Tips:**
- Gebruik verschillende kleuren per draad
- Check polariteit (VCC/GND niet omkeren!)
- Zorg voor stevige verbinding

### Stap 2: Knoppen Aansluiten (Optioneel)

Twee knoppen geven handmatige standbediening — handig als WiFi of MQTT uitvalt.

**Benodigdheden:** 2× drukknop (NO), geen weerstanden

```
Knop OMHOOG:   D5 ────┤ knop ├──── GND
Knop OMLAAG:   D6 ────┤ knop ├──── GND
```

Gebruik de NO-aansluitpunten (Normally Open). De firmware activeert zelf de interne pull-up.

> Zie **WIRING.md → Handmatige Bediening** voor het volledige schema en knop-aanbevelingen.

### Stap 2: LCD Testen (Optioneel maar aanbevolen)

**Upload test sketch:**
```cpp
# include <Wire.h>
# include <LiquidCrystal_I2C.h>

LiquidCrystal_I2C lcd(0x27, 16, 2);

void setup() {
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("LCD Test OK!");
  lcd.setCursor(0, 1);
  lcd.print("ChofuCtrl");
}

void loop() {}
```

**Verwacht resultaat:**
```
╔════════════════╗
║LCD Test OK!    ║
║ChofuCtrl     ║
╚════════════════╝
```

**Als LCD niet werkt:**
- Check jumper wires (goed aangesloten?)
- Probeer I2C adres 0x3F in plaats van 0x27
- Check contrast potentiometer op LCD (schroef op achterkant)

---

## FASE 3: Configuratie en Code Uploaden

### Stap 1: Repository Klonen of Downloaden

```
1. Download of clone de repository
2. Ga naar de map chofu_wp_ff/
```

### Stap 2: Configuratiebestand Aanmaken

```bash
cp chofu_wp_ff/config.h.example chofu_wp_ff/config.h
```

Bewerk `chofu_wp_ff/config.h`:
```cpp
const char* SSID        = "jouw-netwerk";
const char* PASS        = "jouw-wachtwoord";
const char* MQTT_BROKER = "192.168.1.x";
const int   MQTT_PORT   = 1883;
const char* MQTT_USER   = "";   // leeg als geen auth
const char* MQTT_PASS   = "";
```

> `config.h` staat in `.gitignore` en wordt nooit gecommit.

### Stap 3: Firmware Openen

```
1. Dubbelklik chofu_wp_ff/chofu_wp_ff.ino
2. Arduino IDE opent automatisch (inclusief config.h tab)
3. Controleer dat board en port correct zijn ingesteld
```

### Stap 3: Arduino Verbinden

```
1. Sluit Arduino aan via USB
2. Wacht op "USB device connected" (Windows/Mac)
3. Tools → Port → Selecteer COM port (Windows) of /dev/tty.usb* (Mac)
4. Als geen port zichtbaar: installeer CH340 driver (Google: "CH340 driver")
```

### Stap 4: Verificatie (Compileren)

```
1. Klik Verify ✓ icoon (of Sketch → Verify/Compile)
2. Wacht op compilatie (kan 30-60 sec duren)
3. Onderaan moet verschijnen:
   "Done compiling."
   "Sketch uses XXXX bytes (XX%) of program storage space."
```

**Als compilatie fouten:**
```
Foutmelding: "WiFiS3.h: No such file"
→ Controleer: is Arduino UNO R4 WiFi geselecteerd als board?
  Op ESP32 is dit geen probleem (die gebruikt WiFi.h automatisch)

Foutmelding: "config.h: No such file or directory"
→ Maak config.h aan: cp config.h.example config.h

Foutmelding: "Board not found"
→ Installeer het juiste board package (zie Fase 1 Stap 2)
```

### Stap 5: Uploaden

```
1. Klik Upload → icoon (of Sketch → Upload)
2. Wacht op compilatie + upload (1-2 minuten)
3. Tijdens upload knippert RX/TX leds op Arduino
4. Onderaan verschijnt: "Done uploading."
```

**Als upload faalt:**
```
Error: "Port already in use"
→ Sluit Serial Monitor
→ Probeer opnieuw

Error: "Programmer not responding"
→ Check USB kabel (sommige kabels zijn alleen voor laden!)
→ Druk op reset knop Arduino en upload direct daarna

Error: "Access denied"
→ Linux: sudo chmod 666 /dev/ttyUSB0
```

---

## FASE 4: WiFi en MQTT Instellingen

WiFi- en MQTT-credentials worden ingesteld via `config.h` — **niet** via een captive portal. Zie Fase 3 Stap 2 voor de instructies. Upload daarna de firmware opnieuw.

Na een succesvolle upload verbindt de Arduino direct met jouw WiFi en MQTT broker. Er is geen extra configuratiestap nodig.

---

## FASE 5: Verificatie

### Stap 1: Serial Monitor Check

```
1. Arduino IDE → Tools → Serial Monitor
2. Baud rate: 115200 (onderaan rechts)
3. Moet zien:

ChofuCtrl v2.0 — FF modus
EEPROM: lees opgeslagen settings
  FF UA huis:272 emitter:267

WiFi OK! IP: 192.168.1.XXX

MQTT OK!
Discovery F1
Discovery F2
Discovery F3
Systeem operationeel
```

**Als WiFi niet verbindt:**
```
Foutmelding: "WiFi connection failed"
→ Check SSID correct (hoofdlettergevoelig!)
→ Check wachtwoord correct
→ Check 2.4GHz netwerk (geen 5GHz)
→ Plaats Arduino dichter bij router
```

**Als MQTT niet verbindt:**
```
Foutmelding: "MQTT connection failed"
→ Check MQTT broker draait
→ Check IP adres correct
→ Check firewall (poort 1883 open?)
→ Test met MQTT Explorer tool
```

### Stap 2: Web Interface Check

```
1. Open browser
2. Surf naar: http://[arduino-ip]
   (IP zie je in Serial Monitor)
3. Moet zien: Web dashboard met warmtepomp data
```

### Stap 3: LCD Check

De LCD roteert elke 6 seconden door 4 schermen. **Scherm 0 en 3 zijn altijd gelijk; schermen 1 en 2 zijn mode-specifiek.** Zie [docs/LCD.md](docs/LCD.md) voor de volledige referentie.

**Scherm 0 — Altijd: status**
```
╔════════════════╗
║St3 1200W  AAN ║   stand · vermogen · aan/uit
║FF-A   Hz:45   ║   modus · compressorfrequentie
╚════════════════╝
```

**Scherm 1 — Primaire regelinfo (voorbeeld: FF_AUTO)**
```
╔════════════════╗
║K:20.1 >21.0   ║   kamer actual → setpoint
║B: 5.0  UA:272 ║   buiten · geleerd UA_house
╚════════════════╝
```

**Scherm 3 — Altijd: netwerk**
```
╔════════════════╗
║P:60%  DT: 5.1 ║   pomp% · delta_T
║192.168.1.50   ║   IP-adres
╚════════════════╝
```

---

## FASE 6: Home Assistant Integratie

### Stap 1: MQTT Broker Check

**Als je nog geen MQTT broker hebt:**
```
1. Home Assistant → Settings → Add-ons
2. Zoek: "Mosquitto broker"
3. Install
4. Start
5. Configuration tab:
   logins:
     - username: mqtt
       password: [jouw-wachtwoord]
6. Save → Restart add-on
```

### Stap 2: MQTT Integratie

```
1. Settings → Devices & Services
2. Als MQTT niet zichtbaar:
   - Add Integration
   - Zoek: MQTT
   - Broker: 127.0.0.1 (of core-mosquitto)
   - Port: 1883
   - Username/Password: (zoals in Mosquitto config)
```

### Stap 3: Entities Verschijnen (Auto-Discovery)

```
Wacht 2-3 minuten...

Settings → Devices & Services → MQTT → Devices
Moet zien: "ChofuCtrl" (of jouw gekozen naam)

Klik erop → Zie 16+ entities:
✓ ChofuCtrl Aanvoer (sensor)
✓ ChofuCtrl Retour (sensor)
✓ ChofuCtrl Kamer (sensor)
✓ ChofuCtrl Stand (sensor)
✓ ChofuCtrl Power (switch)
✓ ... etc
```

**Als geen entities verschijnen:**
```
1. Check Serial Monitor: "Discovery fase 1/2/3 gestart"
2. Developer Tools → MQTT
3. Listen to: homeassistant/#
4. Moet zien: discovery messages
5. Als niks: check MQTT verbinding Arduino
6. Reset Arduino (discovery opnieuw)
```

### Stap 4: Dashboard Maken

```
1. Overview → Edit Dashboard
2. Add Card → Entities
3. Selecteer entities:
   - sensor.chofu_ctrl_kamer
   - sensor.chofu_ctrl_aanvoer
   - sensor.chofu_ctrl_retour
   - sensor.chofu_ctrl_stand
   - sensor.chofu_ctrl_vermogen
   - switch.chofu_ctrl_power
4. Save
```

---

## FASE 7: Warmtepomp Protocol Verbinden

### BELANGRIJK - LEES DIT EERST!

```
GEVAAR: Verkeerde aansluiting kan hardware beschadigen!

✓ Check spanning: Protocol is 5V (Arduino is 5V tolerant)
✓ Gebruik optioneel isolatie (optocoupler)
✓ Test eerst op een test-setup
✓ Maak foto's voor je begint
```

### Optie A: Direct Verbinden (Simpel maar minder veilig)

**Benodigdheden:**
- 3x Jumper wire
- Schroevendraaier (voor controlbox terminals)

**Aansluiting (op het IC-voetje van de verwijderde chip):**
```
Controlbox          Arduino UNO R4 WiFi
─────────────       ───────────────────
Pad "RX"        →   D0 (RX1)              (pomp → Arduino)
Pad "TX"        →   D1 (TX1)              (Arduino → pomp, via 1kΩ weerstand!)
GND             →   GND
```

**BELANGRIJK:**
- **De RX/TX-labels op het IC-voetje zijn vanuit de verwijderde chip gezien!** De Arduino vervangt die chip: Arduino-TX op pad "TX", Arduino-RX op pad "RX" — dus *niet* gekruist zoals tussen twee losse apparaten. Verkeerd om = polls gaan uit, pomp antwoordt nooit (`JGC timeout: geen frame >2s`).
- **D1 (TX1) MOET via 1kΩ weerstand!** (beschermt de controlbox; zonder weerstand risico op beschadiging)
- Gebruik **D0/D1** (hardware UART, Serial1) — **niet** pin 2/3 (SoftwareSerial crasht de WiFi co-processor)
- Communicatie op **666 baud** (ongebruikelijk maar correct voor het Chofu protocol)

### Optie B: Via Optocoupler (Veilig, aanbevolen)

**Benodigdheden:**
- PC817 optocoupler (2x)
- 220Ω weerstand (2x)
- 1kΩ weerstand (2x)
- Breadboard
- Jumper wires

**Schema:**
```
[Zie WIRING.md voor complete schema]
```

### Verificatie Protocol Communicatie

**Serial Monitor moet tonen:**
```
RX WP: A:35.2 R:30.1 B:8.5 Hz:45 P:60%
TX: Stand 2 naar WP
✓ MQTT data verstuurd
```

**Interval (JGC-protocol):**
- TX (poll): elke ~300 ms, roterend over 4 telegrammen
- RX (antwoord): binnen ~100 ms na elke geldige poll

**Als geen data ontvangen:**
```
Check:
✓ D0/D1 niet verwisseld? (labels IC-voetje zijn vanuit de verwijderde chip!)
✓ Jumper wires goed aangesloten?
✓ Controlbox heeft stroom?
✓ Warmtepomp aan?
✓ GND gemeenschappelijk?
✓ Parser = jgc? (chofu/parser — klassiek krijgt geen antwoord)
✓ Diagnose: zet chofu/cmd/proto_log = 1 en check de 30s-samenvatting,
  of flash sniffer/sniffer.ino voor een ruwe hex-dump zonder WiFi
```

---

## FASE 8: Finale Checks

### Checklist

```
□ Arduino geüpload en opgestart
□ WiFi verbonden (zie Serial Monitor)
□ MQTT verbonden (zie Serial Monitor)
□ LCD toont data
□ Web interface bereikbaar
□ Home Assistant entities zichtbaar
□ Protocol data wordt ontvangen (RX messages)
□ Commando's worden verstuurd (TX messages)
□ Warmtepomp reageert op stand wijzigingen
□ (Optioneel) Knoppen op D5/D6 wisselen stand en modus → HANDMATIG
```

### Test Scenario's

**1. Anna Setpoint Wijzigen:**
```
1. Verhoog Anna van 20.5°C naar 21.0°C
2. Wacht 2-3 minuten
3. Check Serial Monitor: "Kamer fout: 0.5°C"
4. Warmtepomp moet reageren (stand omhoog)
```

**2. Handmatige Controle:**
```
1. Home Assistant → switch.chofu_ctrl_power → Turn On
2. Check Serial Monitor: "Handmatig: Stand 1"
3. Warmtepomp moet starten
4. Turn Off → Warmtepomp moet stoppen
```

**3. Web Interface:**
```
1. Surf naar http://[arduino-ip]
2. Wijzig setpoint
3. Check dat wijziging doorgevoerd wordt
```

---

## Troubleshooting

### Arduino Reboot Loop
```
Symptoom: Arduino reset elke paar seconden

Oorzaken:
- Onvoldoende stroom (slechte USB kabel)
- Kortsluiting in bekabeling
- Verkeerde library versie

Oplossing:
- Gebruik goede USB kabel (met data lijnen!)
- Check alle verbindingen (geen kortsluit?)
- Update libraries naar nieuwste versie
```

### "Onbekend" in Home Assistant
```
Symptoom: Entities tonen "Onbekend"

Oorzaken:
- MQTT topics komen niet aan
- Discovery niet compleet
- MQTT verbinding verbroken

Oplossing:
1. Check Serial Monitor: "✓ MQTT data verstuurd"
2. MQTT Explorer: zie je topics?
3. Reset Arduino (discovery opnieuw)
4. Herstart Home Assistant
```

### Warmtepomp Reageert Niet
```
Symptoom: TX commando's maar geen reactie

Oorzaken:
- Verkeerde bekabeling protocol
- Weerstand ontbreekt op TX
- Controlbox in verkeerde modus

Oplossing:
1. Check bekabeling: Arduino D1 → pad "TX", D0 → pad "RX" (labels zijn
   vanuit de verwijderde chip gezien — bij twijfel: wissel D0/D1)
2. Verifieer 1kΩ weerstand op D1 (TX1)
3. Test met handmatig commando (chofu/cmd/stand)
4. Zet chofu/cmd/proto_log = 1 en check chofu/proto/tx + 30s-samenvatting
```

---

## Hulp Nodig?

1. **Check Serial Monitor** (115200 baud) - meeste info staat daar!
2. **MQTT Explorer** - Zie je topics? Wat zijn de waarden?
3. **GitHub Issues** - Stel vraag met Serial Monitor output
4. **Home Assistant Logs** - Settings → System → Logs

---

## Success!

Als alles werkt zie je:
- ✅ LCD met live data
- ✅ Web interface bereikbaar
- ✅ Home Assistant entities met data
- ✅ Warmtepomp reageert op Anna

**Geniet van je slimme warmtepomp!** 🔥

---

**Volgende stappen:**
- [PID Tuning](PID_TUNING.md) - Optimaliseer regeling
- [MQTT Reference](MQTT_REFERENCE.md) - Alle commando's
- [Home Assistant](HOME_ASSISTANT.md) - Geavanceerde automations
