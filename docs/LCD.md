# LCD Schermen — ChofuCtrl

De LCD 16×2 roteert elke **6 seconden** door 4 schermen.  
**Scherm 0 en 3 zijn altijd gelijk. Schermen 1 en 2 zijn mode-specifiek.**

---

## Scherm 0 — Statussamenvatting (alle modi)

```
╔════════════════╗
║St3 1200W  AAN ║   stand (0–12) · vermogen (W) · WP aan/uit
║FF-A   Hz:45   ║   modus · compressorfrequentie (Hz)
╚════════════════╝
```

| Veld | Betekenis |
|------|-----------|
| `St3` | Compressorstand 0–12 |
| `1200W` | Gemeten vermogen (of tabelwaarde als geen meting) |
| `AAN` / `UIT` | Warmtepomp actief |
| `AUTO` / `FF-A` / `FF-W` / `WATR` / `HAND` | Actieve modus |
| `Hz:45` | Compressorfrequentie uit WP-telegram |

---

## Scherm 3 — Netwerk (alle modi)

```
╔════════════════╗
║P:60%  DT: 5.1 ║   pompsnelheid (%) · temperatuurverschil aanvoer−retour
║192.168.1.50   ║   IP-adres van de controller
╚════════════════╝
```

---

## Schermen 1 & 2 — Mode-specifiek

### AUTO — Kamer PID via stooklijn

**Scherm 1 — Kamer PID kern**
```
╔════════════════╗
║K:20.1 >21.0   ║   kamertemperatuur · setpoint
║PID:45%  F:-0.9║   PID-uitgang · regelafwijking kamer (+ = te koud)
╚════════════════╝
```

**Scherm 2 — Aanvoer & stooklijn**
```
╔════════════════╗
║A:35.2 D:38.2  ║   aanvoer · stooklijn doeltemperatuur
║B: 5.0  R:30.1 ║   buitentemperatuur · retour
╚════════════════╝
```

---

### FF_AUTO — Feedforward kamertemperatuur

**Scherm 1 — Kamer & leerwaarde**
```
╔════════════════╗
║K:20.1 >21.0   ║   kamertemperatuur · setpoint
║B: 5.0  UA:272 ║   buitentemperatuur · geleerd UA_house (W/K)
╚════════════════╝
```

**Scherm 2 — Aanvoer & FF-correctie**
```
╔════════════════╗
║A:35.2  R:30.1 ║   aanvoer · retour
║DT:5.1 FF:+0.5 ║   delta_T · FF integraalcorrectie (in standen)
╚════════════════╝
```

> `FF:+0.5` betekent dat de integraalterm 0,5 stand extra vraagt boven de FF-basisstand.

---

### WATER — Aanvoer PID op vast setpoint

**Scherm 1 — Aanvoer PID kern**
```
╔════════════════╗
║A:35.2  SP:32  ║   aanvoertemperatuur · ingesteld setpoint
║PID:45%  F:+3.2║   PID-uitgang · regelafwijking aanvoer (+ = te warm)
╚════════════════╝
```

**Scherm 2 — Retour & omgeving**
```
╔════════════════╗
║R:30.1 DT: 5.1 ║   retour · delta_T
║B: 5.0  K:20.1 ║   buiten · kamertemperatuur
╚════════════════╝
```

---

### FF_WATER — Feedforward aanvoertemperatuur (extern setpoint)

**Scherm 1 — Aanvoer & leerwaarde**
```
╔════════════════╗
║A:35.2 D:38.2  ║   aanvoer · stooklijn doeltemperatuur (extern SP)
║B: 5.0  UA:267 ║   buiten · geleerd UA_emitter (W/K)
╚════════════════╝
```

**Scherm 2 — Retour & FF-correctie**
```
╔════════════════╗
║R:30.1 DT: 5.1 ║   retour · delta_T
║FF:+0.5  Hz:45 ║   FF integraalcorrectie · compressorfrequentie
╚════════════════╝
```

---

### HANDMATIG — Vaste stand

**Scherm 1 — Warmtepompdata**
```
╔════════════════╗
║A:35.2  R:30.1 ║   aanvoer · retour
║DT: 5.1  Hz:45 ║   delta_T · compressorfrequentie
╚════════════════╝
```

**Scherm 2 — Omgeving & vermogen**
```
╔════════════════╗
║K:20.1  B: 5.0 ║   kamertemperatuur · buiten
║Verm:1200W     ║   huidig vermogen
╚════════════════╝
```

---

## Afkortingen

| Afkorting | Betekenis |
|-----------|-----------|
| `A:` | Aanvoertemperatuur (°C) |
| `R:` | Retourtemperatuur (°C) |
| `K:` | Kamertemperatuur (°C) |
| `B:` | Buitentemperatuur (°C) |
| `D:` | Doeltemperatuur stooklijn (°C) |
| `SP:` | Setpoint water (°C) |
| `DT:` | Aanvoer − retour temperatuurverschil (°C) |
| `UA:` | Geleerd warmtedoorgang-getal (W/K) |
| `FF:` | Feedforward integraalcorrectie (stand) |
| `F:` | Regelafwijking / fout (°C) |
| `P:` | Pompsnelheid (%) |
| `St` | Compressorstand (0–12) |
| `Hz:` | Compressorfrequentie |

---

## LCD uitschakelen

Via MQTT: `chofu/cmd/lcd` → `0` (uit) / `1` (aan)  
In Home Assistant: schakelaar *Chofu LCD*.
