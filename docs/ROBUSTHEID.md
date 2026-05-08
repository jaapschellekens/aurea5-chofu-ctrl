# Robustheidsanalyse — Chofu WP Controller v2.0

Analyse van het gedrag van de firmware bij faalscenario's, gebruikersfouten en randgevallen. Per bevinding is aangegeven of het een **bug** is, een **zwakte** (suboptimaal maar niet gevaarlijk), of gewoon **goed gedrag**.

---

## 1. Stroomuitval en herstart

### Wat werkt goed ✅

- **EEPROM** slaat alle instellingen op: setpoint, PID-parameters, stooklijnparameters, FF UA-waarden en modus. Na herstart worden ze hersteld.
- **HANDMATIG-modus** wordt bewust **niet** hersteld na herstart (`eeprom_load` slaat HANDMATIG over → terugval naar AUTO). Dit voorkomt dat de WP na een stroomstoring onbeheerd op een vaste stand blijft draaien.
- **`ctrl` struct** initialiseert altijd op `stand=0`, `wp_aan=false`, alle integralen=0. De WP staat bij herstart altijd uit.
- **`vorige_stand_wijz_ms = 0`** bij boot: `millis() - 0` is altijd groter dan de hysteresis → de regelaar kan direct reageren zonder te wachten op de 10-minuten-timer.
- **Spike-filter defaults** (`prev_t_supply=25`, `prev_t_outside=5`) zijn realistische startwaarden die bij herstart niet onterecht als spike worden afgewezen.

### Zwakte ⚠️

- **`t_kamer` en `t_kamer_gewenst` starten op 19.0°C (hardcoded).** In de tijd tussen herstart en MQTT-verbinding (~10–15 seconden bij normaal netwerk) regelt de controller al op deze defaults. In de meeste gevallen is dit onschuldig (WP staat toch eerst stil), maar bij een herstart in een warme kamer kan `kamer_fout = 19.0 - 19.0 = 0` ertoe leiden dat de WP even niet inschakelt ook al is het buiten koud.

- **Geen WiFi-herverbinding.** Als de WiFi wegvalt, probeert `mqtt_herverbind()` steeds MQTT te verbinden, maar roept nooit `WiFi.begin()` aan. De herverbinding zal dan altijd mislukken zonder een foutmelding. Zie sectie 2.

---

## 2. WiFi / MQTT uitval

### Wat werkt goed ✅

- **Automatische MQTT-herverbinding** in elke loop-iteratie (`mqtt_herverbind()`).
- **LWT correct ingesteld**: `chofu/status = "offline"` (retain=true, QoS=1) wordt door de broker gestuurd als de verbinding verloren gaat. HA entities gaan netjes op "unavailable".
- **MQTT-watchdog** (standaard 2 uur): als er langer dan `MQTT_WATCHDOG_MS` geen MQTT-berichten binnenkomen, schakeldt de controller terug naar `AUTO` modus. Voorkomt dat de WP onbeheerd in bijv. `FF_WATER` blijft draaien zonder actueel Adam-setpoint.
- **Discovery opnieuw** na herverbinding: HA-entities worden correct gerefreshed.

### Bug 🐛

- **Geen WiFi-herverbinding.** `mqtt_herverbind()` roept `mqttClient.connect()` aan, maar als WiFi weg is, werkt dit niet. Er is geen `WiFi.disconnect()` / `WiFi.begin()` logica. Bij een langdurige WiFi-onderbreking (router herstart, etc.) komt de Arduino er nooit meer uit zonder handmatige herstart.

  **Risiconiveau:** matig. De WP blijft gewoon draaien op de laatste staat en de regelaar werkt autonoom door op basis van seriëel data van de WP — maar er is geen MQTT-zicht en geen remote-bediening meer.

  **Oplossing:** Voeg toe in `mqtt_herverbind()`:
  ```cpp
  if(WiFi.status() != WL_CONNECTED){
    WiFi.disconnect();
    WiFi.begin(SSID, PASS);
    delay(5000);
    return;
  }
  ```

---

## 3. Vorst

### Wat werkt goed ✅

- Vorstbeveiliging actief in `AUTO`, `FF_AUTO` en `WATER`: als `t_outside < T_VORST` (standaard 4°C) wordt de WP op minimumstand 1 gehouden, ook als de kamer op temperatuur is.
- COP-formule heeft een vloer van 1.0 en deelt nooit door nul (`dT < 1.0f → return 1.0f`).
- `T_VORST` is instelbaar via MQTT met range-check (−10 tot 10°C).

### Bug 🐛

- **Vorstbeveiliging ontbreekt in `FF_WATER` modus.** In `pas_ff_aan()` staat:
  ```cpp
  if(!is_water && t_outside < T_VORST) nieuwe_stand_i = max(1, nieuwe_stand_i);
  ```
  De conditie `!is_water` sluit FF_WATER (en WATER, maar die zit in een aparte tak) uit. In FF_WATER kan de WP bij vorst dus helemaal uitgeschakeld worden als het Adam-setpoint laag is of de aanvoertemp al op setpoint zit.

  Vergelijk met WATER-modus (regel 635): daar staat de vorstbeveiliging wél correct.

  **Oplossing:** Voeg toe in `pas_ff_aan()` voor de hysteresis/nieuw-stand sectie:
  ```cpp
  if(t_outside < T_VORST) nieuwe_stand_i = max(1, nieuwe_stand_i);
  ```
  (Zonder `!is_water` conditie.)

---

## 4. Te hoge aanvoertemperatuur / oververhitting

### Wat werkt goed ✅

- **`SUPPLY_MAX` noodstop** (standaard 60°C): `pas_pid_aan()` checkt dit als eerste stap, vóór alle modus-logica. Geldt dus voor alle modi inclusief FF.
- **Aanvoer-spike-filter**: spikes van meer dan 10°C worden afgewezen en als alert gemeld. De regelaar reageert niet op meetruis.
- **`STOOKLIJN_UIT_GRENS`**: als het buiten warm genoeg is, gaat de WP automatisch uit.

### Zwakte ⚠️

- **In FF-modi geen bovenbegrenzing op berekende `P_nodig`**. Als UA_house door een bug of slechte meting erg hoog wordt (max. 500 W/K), en het is −10°C buiten met setpoint 21°C, dan is P_nodig = 500 × 31 = 15.5 kW — veel meer dan de WP kan leveren. `ff_stand_voor_vermogen()` kiest dan stand 8. Dat is het maximum en daarmee onschadelijk, maar een extreme UA-waarde kan leiden tot permanent volledig vermogen.

---

## 5. Gebruikersfouten via MQTT

### Wat werkt goed ✅

- De meeste commandos hebben range-validatie: stooklijn, setpoint, UA-waarden, t_kamer_gewenst, anna/setpoint en anna/temperatuur zijn allemaal beperkt.
- Ongeldige payload (`payload.toFloat()` op lege string) geeft 0.0 — wordt afgevangen door de range-check.

### Bug 🐛

- **Kp, Ki, Kd hebben géén range-validatie:**
  ```cpp
  else if(topic == "chofu/cmd/kp"){ Kp = payload.toFloat(); eeprom_save(); }
  else if(topic == "chofu/cmd/ki"){ Ki = payload.toFloat(); eeprom_save(); }
  else if(topic == "chofu/cmd/kd"){ Kd = payload.toFloat(); eeprom_save(); }
  ```
  Een lege payload of `"reset"` → `toFloat()` = 0.0 → **Kp=0, Ki=0, Kd=0 wordt opgeslagen in EEPROM.** De PID doet dan niets meer. Een negatieve Ki leidt tot integraalopbouw in de verkeerde richting (instabiliteit).

  **Oplossing:** Voeg range-check toe:
  ```cpp
  else if(topic == "chofu/cmd/kp"){ float v=payload.toFloat(); if(v>=0.1&&v<=100) { Kp=v; eeprom_save(); } }
  else if(topic == "chofu/cmd/ki"){ float v=payload.toFloat(); if(v>=0.0&&v<=1.0)  { Ki=v; eeprom_save(); } }
  else if(topic == "chofu/cmd/kd"){ float v=payload.toFloat(); if(v>=0.0&&v<=10.0) { Kd=v; eeprom_save(); } }
  ```

### Zwakte ⚠️

- **Modus-wisseling via webinterface** voert `ctrl.reset_pid()` en `ctrl.reset_ff()` uit, maar roept **niet** `ctrl.koude_start()` aan. MQTT doet dat wél. Dit betekent dat na een modus-wisseling via de webpagina de hysteresis-timer niet gereset wordt — de WP kan direct van stand wisselen als de timer al verlopen was. Minor inconsistentie, niet gevaarlijk.

---

## 6. Verlies van serieel contact met de WP

### Wat werkt goed ✅

- Heartbeat: als er langer dan 5 seconden geen serieel telegram binnenkomt, stuurt de Arduino het huidige stand-telegram opnieuw. Dit houdt de WP actief op de ingestelde stand.
- Checksumvalidatie: corrupte telegrams worden afgewezen.

### Zwakte ⚠️

- **Geen watchdog bij langdurig serieel verlies.** Als de WP-controlbox crasht of de kabel loslaat, worden `t_supply`, `t_return`, `t_outside` en `comp_hz` nooit meer bijgewerkt — ze blijven op de laatste bekende waarden staan. De regelaar blijft doorwerken op verouderde data zonder dat dit als fout wordt gesignaleerd.
- **Spike-filter heeft geen timeout.** Als de WP herstart op een heel andere aanvoertemperatuur dan de laatste meting (bijv. na defrost), wordt de eerste meting als spike afgewezen en de waarde niet bijgewerkt. Pas bij de volgende meting (die binnen 10°C van de vorige ligt) wordt de waarde geaccepteerd. In de tussentijd draait de regelaar op een verouderde aanvoertemp.

---

## 7. Koelingsmodus

### Wat werkt goed ✅

- Koeling wordt automatisch uitgeschakeld als `t_outside < KOELING_MIN_BUITEN` (standaard 18°C). Voorkoming dat koeling actief is bij koude buitentemperaturen.
- De koeling-check staat vóór de FF-tak in `pas_pid_aan()` — geldt voor alle modi.
- PID wordt gereset bij koeling-activering.

---

## 8. Lange uptime (millis() overflow na ~49 dagen)

### Goed afgehandeld ✅

- `koude_start()` gebruikt `nu - 700000UL` expliciet als unsigned — correct bij wraparound.
- Alle hysteresis-vergelijkingen gebruiken `(uint32_t)` cast — unsigned aftrekking werkt correct bij overflow.
- `millis() - vorige_*_ms > threshold` patroon is overal consistent — ook correct bij overflow.

---

## Overzicht

| # | Scenario | Ernst | Status |
|---|----------|-------|--------|
| 1 | Stroomuitval → herstart | Laag | ✅ Robuust |
| 2 | WiFi-uitval (niet alleen MQTT) | Matig | 🐛 Bug: geen WiFi reconnect |
| 3 | Vorstbeveiliging in FF_WATER | Matig | 🐛 Bug: ontbreekt |
| 4 | Kp/Ki/Kd zonder range-check | Hoog | 🐛 Bug: 0-waarden worden opgeslagen |
| 5 | Serieel verlies WP | Laag | ⚠️ Geen watchdog/alarm |
| 6 | Spike-filter na WP-herstart | Laag | ⚠️ Tijdelijk verouderde data |
| 7 | Te hoge aanvoer / noodstop | Laag | ✅ Robuust (SUPPLY_MAX) |
| 8 | Webinterface modus-wisseling | Laag | ⚠️ Inconsistente hysteresis reset |
| 9 | millis() overflow | Geen | ✅ Correct afgehandeld |
| 10 | MQTT-watchdog | Laag | ✅ Robuust (2 uur → AUTO) |
