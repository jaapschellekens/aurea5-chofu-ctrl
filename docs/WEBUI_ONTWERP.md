# Ontwerp — Web-interface met tabs (alle instellingen)

Status: **ontwerp / ter review** (nog geen code)
Doel: alle ~48 instelbare parameters via de ingebouwde webserver kunnen aanpassen,
logisch gegroepeerd in tabs, zonder de eenvoud/robuustheid van de MCU-webserver te
verliezen en zonder uit de pas te lopen met de MQTT-commando's.

---

## 1. Uitgangspunten

- Draait op de MCU-webserver (`web.cpp`): één `WiFiClient` per keer, antwoord wordt
  als string geprint. Beperkt RAM → HTML/JSON **streamen** (veel `client.print`),
  geen grote `String` opbouwen.
- **Geen JavaScript**: pure HTML + CSS (besluit). Tabs en navigatie server-side,
  geen AJAX. Dit is ook het lichtst voor de MCU.
- **Rustig** en licht: alleen de Status-tab ververst automatisch, met een ruim
  interval; instellingen-tabs verversen niet (geen onderbreking tijdens invoer).
- **Zelfstandig**: CSS inline in de pagina, geen externe CDN (werkt offline/LAN).
- **Geen dubbele waarheid**: web en MQTT moeten exact dezelfde validatie en
  neveneffecten (EEPROM-opslaan, klep schakelen, koude-start) gebruiken.
- Moet naast HA/MQTT blijven werken; web-wijzigingen publiceren ook de nieuwe
  status zodat HA meeloopt.
- Coëxistentie met meerdere ontwikkelaars (Copilot): nieuwe instelling toevoegen
  op zo min mogelijk plekken.

---

## 2. Tab-indeling

| Tab | Inhoud (parameters) |
|-----|----------------------|
| **Status** | (alleen-lezen) wp aan/uit, modus, stand + max, aanvoer/retour/ΔT/buiten/kamer, doel-setpoint, vermogen, COP/Hz, SWW-status + klep, bron, defrost, uptime, IP |
| **Bediening** | `modus`, `power`/`stand` (handmatig), `koeling`, `sww`, `force_start` |
| **Stooklijn** | `stooklijn_basis`, `stooklijn_grens`, `stooklijn_factor`, `stooklijn_uit`, `stooklijn_aan`, `t_vorst` |
| **Water & kamer** | `water_setpoint`, `water_sp_min`, `kamer_in_water`, `kamer`, `kamer_setpoint` |
| **Regeling — PID** | `kp`/`ki`/`kd`, `kp_water`/`ki_water`/`kd_water` |
| **Regeling — Feedforward** | `ff_ua_house`, `ff_ua_emitter`, `ff_save` |
| **Koeling** | `koeling`, `koeling_min_buiten`, `supply_min` |
| **Tapwater (SWW)** | `sww`, `sww_setpoint`, `sww_max_stand` (+ klep-status) |
| **Grenzen & systeem** | `supply_max`, `max_stand`, `lcd`, `proto_log`, `seriallog` |
| **Geavanceerd** | `pid_interval`, `hyst_slow`/`hyst_fast`/`hyst_down`, `auto_hyst_down`, `ff_min_off`, `ff_restart_coast`, `ff_lookahead`, `ff_thermal_min_off`, `ff_afschakel`, `koeling_afschakel`, `koel_deadband` |

(10 tabs; "Status" is de standaard-tab. De zelden gebruikte timing-/regelinternals
staan onder **Geavanceerd** zodat de gewone tabs rustig blijven. De `sim`-topics
(simulatie) komen **niet** in de web-UI — die blijven alleen via MQTT.)

---

## 3. Architectuur — drie pijlers

### 3.1 Gedeelde setter `set_param()` (kern van het ontwerp)

De grote `if/else if`-keten in `mqtt_ontvang()` bevat nu alle validatie en
neveneffecten. Die wordt herbruikt door de web-UI door hem te extraheren naar één
functie:

```cpp
// mqtt.cpp / nieuw: settings.cpp
bool set_param(const String& key, const String& val);   // true = toegepast
```

- `mqtt_ontvang()` haalt het laatste padsegment uit het topic (na `/cmd/`) en roept
  `set_param(segment, payload)` aan.
- De web-handler roept voor elk ontvangen formulierveld `set_param(name, value)` aan.

Eén bron van waarheid → web en MQTT gedragen zich identiek, en een nieuwe instelling
toevoegen hoeft maar op één plek. Acties zonder waarde (`ff_save`, `force_start`)
worden gewoon keys met een dummy-waarde.

### 3.2 Settings-metadata tabel (rendering)

Voor het *tekenen* van de formuliervelden een compacte tabel (PROGMEM/const):

```cpp
enum WType { W_NUM, W_TOGGLE, W_SELECT, W_ACTION, W_RO };
struct WebSetting {
  const char* key;     // == MQTT cmd-segment / form-name
  const char* label;   // weergavenaam
  uint8_t     tab;     // index in TABS[]
  WType       type;
  float       vmin, vmax, vstep;
  const char* unit;    // "°C", "%", "" ...
};
static const WebSetting SETTINGS[] PROGMEM = { ... };
```

De webpagina genereert per tab automatisch de juiste invoer (number/checkbox/
select/knop) uit deze tabel. De huidige waarde komt uit de JSON-status (§3.4) of
uit een kleine `get_param(key)`. Zo blijft de UI compleet zonder ~48× handgeschreven
`client.print`-regels, en is hij makkelijk uit te breiden (één regel in de tabel).

> `modus` en `sww`/`koeling` zijn `W_SELECT`/`W_TOGGLE`; `power`/`stand`,
> `force_start`, `ff_save` zijn `W_ACTION`; temperaturen op de Status-tab `W_RO`.

### 3.3 Tabs — server-side, één tab per request (geen JS)

De actieve tab zit in de URL: `GET /?tab=regeling`. De navigatie is een rij gewone
links; de server rendert **alleen de gekozen tab** (plus de altijd-zichtbare
nav-balk). Voordelen passend bij de eisen:

- **Geen JavaScript** nodig — de tab is gewoon een link.
- **Licht voor de MCU**: per request wordt maar één tab (een handvol velden)
  opgebouwd i.p.v. alle ~48 — kleinere responses, minder RAM/tijd.
- **Tab blijft behouden na opslaan**: het formulier post terug naar dezelfde
  `?tab=...` (zie §4).

```html
<nav>
  <a href="/?tab=status"   class="active">Status</a>
  <a href="/?tab=stooklijn">Stooklijn</a>
  <a href="/?tab=geavanceerd">Geavanceerd</a> ...
</nav>
<!-- alleen de inhoud van de gekozen tab -->
```

De actieve tab krijgt server-side de class `active` (CSS-accent). Standaard (geen
`?tab=`) → Status.

### 3.4 Status verversen — alleen die tab, rustig interval

Alleen de **Status-tab** ververst zichzelf met een pure HTML meta-refresh:

```html
<!-- alleen meegegeven als tab==status -->
<meta http-equiv="refresh" content="30; url=/?tab=status">
```

Instellingen-tabs krijgen **geen** refresh → je wordt niet onderbroken tijdens het
invullen. Interval ruim (voorstel **30s**, instelbaar als constante). Geen JSON/AJAX
nodig; de Status-tab is sowieso klein dus de 30s-herlaad is licht.

---

## 4. Opslaan — gewoon HTML-formulier per tab (geen JS)

Elke instellingen-tab is één `<form method="get" action="/">` met een verborgen
veld `<input type="hidden" name="tab" value="regeling">` en een Opslaan-knop. Bij
submit stuurt de browser alle velden van die tab plus `tab=regeling` → de server
past ze toe via `set_param()` en rendert **dezelfde tab** opnieuw (tab blijft dus
behouden, zonder JS). Volle reload, maar alleen van die kleine tab.

De bestaande `GET /?param=value`-afhandeling blijft de basis; ze roept nu per veld
`set_param(name, value)` aan i.p.v. losse inline-validatie. Na toepassing publiceert
de firmware de nieuwe status op MQTT (zoals nu via `data_sturen_gevraagd`) zodat HA
meeloopt.

> Toggles (`koeling`, `sww`, `lcd`, `proto_log`, `seriallog`) = `<select>` of een
> checkbox; acties (`force_start`, `ff_save`) = aparte submit-knoppen. Alles werkt
> met standaard form-controls, geen script.

---

## 5. Randvoorwaarden / valkuilen

- **RAM**: stream alles (`client.print` per stuk); bouw geen volledige pagina in één
  `String`. De settings-tabel in PROGMEM houdt het flash-, niet RAM-gebruik laag.
- **Eén client tegelijk**: de huidige 2s read-timeout blijft; AJAX-calls zijn kort.
- **Auth**: de webserver heeft nu geen wachtwoord. Bij plaatsing buiten een vertrouwd
  LAN: minimaal HTTP Basic-auth overwegen (los ontwerppunt).
- **Volgorde met JGC**: een grote pagina opbouwen kost tijd in `handle_web_client()`;
  dat draait al buiten het RX-pad, maar houd de responses klein (tabs laden client-side,
  status via aparte korte JSON).

---

## 6. Incrementeel migratieplan

1. **`set_param()` extraheren** uit `mqtt_ontvang()` en MQTT erop laten aanroepen
   (gedragsneutraal; puur refactor). Direct winst: web kan later hergebruiken.
2. **`?tab=`-routing** toevoegen in `handle_web_client()`: nav-balk + alleen de
   gekozen tab renderen; bestaande velden in de juiste tab plaatsen. Meta-refresh
   alleen op de Status-tab (30s); de huidige `location.href='/'`-reload vervalt.
3. **Settings-metadata tabel** invoeren en de resterende ~32 parameters toevoegen,
   formuliervelden datagedreven genereren (per tab).
4. **Opslaan via `set_param()`**: de web-handler verwerkt alle formuliervelden via
   de gedeelde setter i.p.v. losse inline-validatie.
5. (optioneel) Wachtwoordbeveiliging.

Elke stap is op zichzelf bruikbaar en testbaar; je hoeft niet alles tegelijk om te
bouwen, wat ook prettig is met meerdere ontwikkelaars op `main`.

---

## 7. Besluiten & open punten

Besloten:
- **Geen JavaScript** → server-side tabs (`?tab=`) + meta-refresh.
- **Rustig interval**: Status-tab ververst elke **30s**, overige tabs niet.
- **Geavanceerd-tab** voor de zelden-gebruikte timing/regelinternals.
- **Licht voor de MCU**: per request maar één tab renderen.
- **`sim` niet in de UI** — simulatie blijft alleen via MQTT.
- **Geen wachtwoordbeveiliging** (gebruik op vertrouwd LAN).

Geen open punten meer — klaar om te implementeren volgens het migratieplan (§6).
