# Ervaringen & Valkuilen — Aurea 5 / Chofu Warmtepomp Controller

Opgeslagen bevindingen uit de ontwikkeling van deze firmware. Raadpleeg dit bestand aan het begin van een nieuwe sessie om bekende problemen te vermijden.

---

## ESP32-specifieke valkuilen

### GPIO6–11 zijn NIET beschikbaar (SPI-flash)
Op de standaard ESP32-D0WD (DevKit V1, WROOM) zijn GPIO6 t/m GPIO11 intern verbonden aan de SPI-flash chip. `pinMode(6, ...)` of `digitalRead(6)` veroorzaakt onmiddellijk een **TG1WDT_SYS_RESET (rst:0x8)** bootloop.

**Veilige GPIO's voor IO:** 4, 5, 12, 13, 14, 15, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33, 34, 35, 36, 39.

> Ons probleem: `BTN_DOWN = 6` in `knoppen.h` crashte de ESP32 direct na NVS-load.  
> Opgelost: board-conditionele defines in `knoppen.h`, ESP32 gebruikt GPIO4/13.

---

### EEPROM bestaat niet op ESP32 core 3.x
ESP32 Arduino core 3.x heeft `<EEPROM.h>` verwijderd. Gebruik in plaats daarvan `Preferences` (NVS, Non-Volatile Storage in flash).

**Oplossing in dit project:** `eeprom.cpp` is gesplitst met `#if defined(ARDUINO_UNOR4_WIFI)` — UNO R4 gebruikt EEPROM, ESP32 gebruikt `Preferences` met namespace `"chofu"`.

Sleutelnamen in Preferences mogen **maximaal 15 tekens** zijn.

---

### LCD / I2C hangt zonder hardware
`LiquidCrystal_I2C.init()` blokkeert de I2C-bus indefinitely als er geen LCD is aangesloten op ESP32 (geen hardware I2C timeout). Dit triggert de Task Watchdog Timer.

**Oplossing:** `USE_LCD = false` standaard op ESP32 in `types.h`. Alle bare `lcd.*` aanroepen buiten een `if(USE_LCD)` blok zijn gewrapt met `#if USE_LCD`.

---

### Upload mislukt met esptool v5.2.0 (baud 921600)
esptool v5.2.0 crasht bij het wisselen naar 921600 baud (`StopIteration`).

**Oplossing:** Arduino IDE → Tools → Upload Speed → **115200**.

---

### `#pragma once` onbetrouwbaar op ESP32 toolchain
De ESP32 toolchain behandelt `#pragma once` niet altijd correct bij `.cpp`-bestanden die hetzelfde header includen → "multiple definition of enum class Modus".

**Oplossing:** Klassieke `#ifndef GUARD_H / #define GUARD_H / ... / #endif` in alle headers.

---

### Serial1 op ESP32 vereist expliciete pinnen
```cpp
// UNO R4:
chofuSerial.begin(666);

// ESP32:
chofuSerial.begin(666, SERIAL_8N1, CHOFU_RX_PIN, CHOFU_TX_PIN);
// standaard: CHOFU_RX_PIN=16, CHOFU_TX_PIN=17 (UART2)
```

---

## Arduino UNO R4 WiFi-specifieke valkuilen

### SoftwareSerial crasht de WiFi co-processor
Gebruik uitsluitend `Serial1` (hardware UART, pinnen D0/D1) voor Chofu-communicatie. SoftwareSerial conflicteert met de WiFi co-processor van de UNO R4 en veroorzaakt herstart-loops.

### WiFi library heet anders
`WiFiS3.h` (UNO R4) vs `WiFi.h` (ESP32) — zit al in `#if defined(ARDUINO_UNOR4_WIFI)` guards.

---

## Hardware-communicatie valkuilen (juni 2026, opgelost)

### TX/RX-labels op het IC-voetje zijn vanuit de verwijderde chip
De Arduino *vervangt* de chip en moet pad "TX" met zijn eigen TX (D1) aansturen en pad "RX" op D0 lezen — omgekeerd aan de gebruikelijke TX→RX conventie tussen twee apparaten. Verkeerd om = TX gaat uit maar de pomp antwoordt nooit (herhaald `JGC timeout: geen frame >2s, stuur TX`).

### De pomp antwoordt alleen op geldige JGC-polls (master/slave)
Met de chip verwijderd is de lijn stil totdat de Arduino correcte JGC-telegrammen (CRC-CCITT) stuurt. Een passieve sniffer ziet daardoor ook níets — dat is geen defect.

### De "eindnul" bestaat niet op de UNO R4 (~100% RX-verlies)
De afsluitende `0x00` die `jgc.ino` per frame verwacht is **geen protocolbyte maar een AVR-artefact**: na het laatste frame-byte laat de pomp de lijn los (break-conditie), en de AVR-UART van de Mega levert dat af als databyte `0x00`. De Renesas-UART van de UNO R4 filtert framing-errors weg — die byte komt dus nooit aan, waardoor de parser eeuwig op de terminator wachtte (eerst eindnul-fouten + TX dwars door frames, na de wachtfix mid-frame aborts). Bewijs via `sniffer/sniffer.ino` (pollen + hex-dump zonder WiFi): elk pompframe is exact `lenbyte` bytes lang, nooit een trailing `00`, ook niet na 270 ms stilte. Fix: payload = `msg_len − 4` bytes (incl. 2 CRC-bytes), frame is compleet na de payload, geen terminator-state. NB: `jgc.ino` leest door een index-quirk (`index++` na de header) feitelijk ook maar `DataLength−1` payloadbytes — de wire-layout is dus in beide implementaties gelijk.

### Half-duplex lijn met TX-echo
Alle eigen TX-bytes komen als echo terug op RX (zichtbaar in de sniffer-dump). De parser negeert ze doordat ze geen `0x91` bevatten, maar: `jgc_is_ontvangend` moet waar blijven zolang de parser mid-frame zit (zoals `IsReceiving` in jgc.ino), TX wacht ≥99 ms na de laatste ontvangen byte, en een mid-frame timeout (600 ms) voorkomt permanente TX-blokkade bij een gestoord frame.

### Spike-guard deadlock op buitentemperatuur
De pomp stuurt bij koude start 0,0 °C buiten. Dat werd geaccepteerd (|0−5| = 5, net niet > 5), waarna de echte waarde (bijv. 21,6) voor eeuwig als spike werd afgewezen omdat `prev_t_outside` bij afwijzing niet werd bijgewerkt. Symptoom: `chofu/outside` blijft op 0 staan + herhaalde `JGC spike buiten`-alerts. Fix: `prev` wordt nu óók bij afwijzing bijgewerkt — een eenmalige glitch wordt nog steeds gefilterd, een aanhoudende waarde wordt bij het tweede frame geaccepteerd.

---

## MQTT-valkuilen

### ArduinoMqttClient trunceert stil op 256 bytes
Altijd `pl.length()` meegeven aan `mqttClient.beginMessage(topic, pl.length())`. Zonder expliciete lengte trunceert de library stil na 256 bytes — discovery-payloads zijn vaak groter.

### Home Assistant `unit_of_measurement` moet exact kloppen
Gebruik `"°C"` (UTF-8 graadbool, 2 bytes) niet `"C"`. HA valideert de unit tegen `device_class` en slaat de entity **stil** over bij een mismatch. Let bij `pl.length()` op de extra byte van het graadbool-teken.

### Retain-regels
| Topic | Retain |
|---|---|
| `homeassistant/*/config` (discovery) | `true` |
| `chofu/status = "online"` | `true` |
| LWT `chofu/status = "offline"` | `true` (via `beginWill`) |
| `chofu/sim/*` en `anna/temperatuur` (simulator) | `true` — anders verliest Arduino de waarden na MQTT reconnect |

---

## EEPROM magic byte protocol

`EEPROM_MAGIC` begint op `0xAD`. Bij elke toevoeging van een nieuw persistent veld:
1. Verhoog `EEPROM_MAGIC` (huidige waarde: `0xB3`)
2. Voeg een `isnan()`-check toe ná `EEPROM.get()` / `prefs.getFloat()` — een NaN-waarde passeert de range-check `< 50 || > 500` stil.

---

## Regelaar & control

### Gekalibreerde parameters (KGE-geoptimaliseerd, niet zomaar wijzigen)
```
PID:  Kp=19.9  Ki=0.084  Kd=0.036
      (in NVS/EEPROM als 75.0/0.800/0.01 — dit zijn de MQTT-waarden, niet de interne schaling)

FF:   UA_house=272.5 W/K   UA_emitter=267.5 W/K
      FF_KI_AUTO=0.026      FF_COAST_AUTO=0.30°C
      FF_KI_WATER=0.017     FF_COAST_WATER=2.5°C
Stooklijn: grens=15°C  factor=0.68
```

### FF_AUTO is grotendeels zelflerend
Stuur bij comfort-klachten **eerst** `ff_ua_house` bij via MQTT (`chofu/cmd/ff_ua_house`) voordat je stooklijn-parameters aanpast. UA_house heeft meer effect dan de stooklijn in FF-modi.

### Predictieve terugschakeling
De ringbuffer in `ControllerState` (21 slots × 60s) slaat kamertemperatuur op voor de sliding-window afgeleide. Wordt gebruikt om de WP vroegtijdig af te schakelen voordat de kamer de setpoint overschrijdt.

---

## Hardware

### Otronic ESP32 DevKit V1 (AH313)
- Chip: ESP32-D0WD-V3 rev 3.1
- Flash: 4MB
- MAC: 70:4b:ca:83:6f:e4
- Upload: **115200 baud** (921600 geeft esptool-crash)
- Boot-knop: houd ingedrukt tijdens upload-start als auto-reset niet werkt
- Geen PSRAM op dit board

### Chofu/Atlantic Aurea 5
- Seriele communicatie: **666 baud**, 8N1, half-duplex met TX-echo
- JGC multi-frame formaat: frames exact `lenbyte` bytes, 4 IDs, CRC-CCITT (residu 0), GEEN terminator
- Command byte: `0x19`, Status byte: `0x91`; koeling: telegram 19-2 byte 3 = 0x02
- Vermogensreeks standen 0–12: `{0, 240, 420, 640, 850, 1050, 1250, 1450, 1550, 1650, 1700, 1750, 1800}` W

### I2C LCD (alleen UNO R4)
- Adres: `0x27` (controleer met I2C-scanner als LCD niet reageert — sommige modules gebruiken `0x3F`)
- `lcd.init()` tweemaal aanroepen is bewust (initialiseert de I2C expander correct)

---

## Simulator

```bash
python python/wp_simulator.py --host <MQTT_BROKER_IP> --modus ff_auto --outside 3.0 --speed 6
```

- `--modus` publiceert met `retain=True` — zonder retain wint een oud retained commando van de broker
- Simulatieparameters: `UA_house=263 W/K`, `C_th=12.5 MJ/K`
- Publiceert op `chofu/sim/supply`, `chofu/sim/outside`, `chofu/sim/return`, `chofu/sim/power`

---

## Plugwise Adam lokale REST API

### HTTP/1.1 verplicht
De Adam weigert HTTP/1.0 met `505 HTTP Version Not Supported`. Gebruik altijd `HTTP/1.1`.

### Chunked encoding — geen probleem voor streaming parser
HTTP/1.1 gebruikt chunked transfer encoding. De chunk-headers (bijv. `3FA2\r\n`) bestaan uit korte hex-strings en verstoren een KMP-achtige streaming zoekfunctie niet, zolang een chunk-grens niet precies in het midden van de gezochte XML-tag valt (kans verwaarloosbaar bij chunks van 4KB+).

### XML-structuur
- Endpoint: `GET /core/domain_objects` — één XML-response van ~215 KB
- Auth: HTTP Basic, gebruikersnaam altijd `smile`, wachtwoord = 8-teken stickercode
- **Niet gesorteerd** — appliances en locations zijn volledig door elkaar
- `<location id='UUID'/>` (self-closing) = referentie binnenin een appliance-element
- `<location id='UUID'>` (niet self-closing) = echte zone (top-level element)
- `intended_boiler_temperature` staat op ~32 KB (~14% van de XML) — vóór de zone-elementen
- `maximum_boiler_temperature` is het plafond van de Adam-stooklijn, **niet** het actieve setpoint

### Timings (ESP32, WiFi)
- Connectie + headers: ~200 ms
- Lezen tot `intended_boiler_temperature` (~32 KB): ~600–900 ms totaal
- Totale XML (~215 KB): niet nodig voor setpoint

### Betekenis van `intended_boiler_temperature`
- Waarde `0` = geen warmtevraag (Adam heeft WP/ketel uitgeschakeld)
- Waarde `> 0` = gewenste aanvoertemperatuur in °C die Adam via OpenTherm naar de ketel stuurt
- Gebruik als `t_water_gewenst` in `WATER`/`FF_WATER` modus van de Chofu controller

### Integratie-aanwijzing
```cpp
float sp = fetch_adam_setpoint();
if (!isnan(sp)) {
    t_water_gewenst = sp;           // overschrijf lokaal setpoint
    if (sp == 0) ctrl.zet_uit();    // geen warmtevraag: WP uit
}
```
