#!/usr/bin/env python3
"""Analyse van een Home Assistant-geschiedenisexport (ESPHome aurea-wp + Anna/Adam).

Gebruik:
    python3 ha_history_analyse.py history.csv          # of .xlsx (openpyxl nodig)

Verwacht (via HA -> Geschiedenis -> Download data) in elk geval:
    aanvoer, retour, doel_aanvoer, stand, vermogen, buiten (WP-voeler),
    statusbyte_buitenunit_id3_byte_8, anna_temperatuur, anna_setpoint,
    en een buitensensor (cresta_4802_temp). Ontbrekende entiteiten worden NaN.

Uitvoer:
    1. lijst defrosts (statusbyte bit 0)
    2. uurtabel: kamer t.o.v. setpoint, buiten, aanvoer t.o.v. doel, stand,
       % van het uur op stand >= 9, elektrisch vermogen, defrosts
    3. warmteverlies: geschatte warmteafgifte (P_el x COP) per graad
       binnen-buiten, en een extrapolatie naar -5 en -10 graden

Zie docs/ANALYSE_WINTER_2026.md voor de achtergrond en de eerste resultaten.
"""
import bisect
import csv
import datetime as dt
import math
import struct
import sys

UTC_OFFSET = dt.timedelta(hours=1)   # HA exporteert UTC; Nederland in de winter = UTC+1
D = dt.timedelta

# Waardebereik per entiteit; gebruikt bij het herstellen van Excel-getallen.
RANGES = {'aanvoer': (0, 70), 'retour': (0, 70), 'doel_aanvoer': (10, 70), 'vermogen': (0, 4000),
          'buiten': (-25, 40), 'stand': (0, 12)}


def f32(x):
    return struct.unpack('f', struct.pack('f', x))[0]


def herstel_excel(key, v, prev):
    """Excel (NL-locale) verliest bij float32-waarden de decimale komma:
    28.7000007629395 wordt 287000007629395. Kies de plaats van de komma waarbij
    de waarde een float32-afgeronde waarde met 1 decimaal is, binnen het bereik."""
    if not isinstance(v, (int, float)) or abs(v) < 1e6:
        return v
    sgn = -1 if v < 0 else 1
    s = str(int(abs(v)))
    lo, hi = RANGES.get(key, (-1e9, 1e9))
    cands = [sgn * int(s) / 10 ** (len(s) - k) for k in range(0, 5)]
    cands = [c for c in cands if lo <= c <= hi] or cands
    exact = [c for c in cands if abs(float('%.15g' % f32(round(c, 1))) - c) <= 1e-12 * max(1, abs(c))]
    cands = exact or cands
    return min(cands, key=lambda c: abs(c - prev)) if prev is not None else cands[0]


def lees(path):
    if path.endswith('.xlsx'):
        import openpyxl
        rows = openpyxl.load_workbook(path, read_only=True).active.iter_rows(values_only=True)
        next(rows)
    else:
        rows = ((r['entity_id'], r['state'], r['last_changed']) for r in csv.DictReader(open(path)))
    ser, prev = {}, {}
    for e, v, t in rows:
        key = e.split('.')[-1].replace('ketelhok_', '').replace('aurea_warmtepomp_', '')
        v = herstel_excel(key, v, prev.get(key))
        try:
            v = float(v)
            prev[key] = v
        except (TypeError, ValueError):
            v = None
        ts = dt.datetime.fromisoformat(str(t).replace('Z', '+00:00')) + UTC_OFFSET
        ser.setdefault(key, []).append((ts, v))
    for k in ser:
        ser[k].sort(key=lambda x: x[0])
    return ser


class Reeks:
    def __init__(self, ser):
        self.ser = ser
        self.tijden = {k: [x[0] for x in s] for k, s in ser.items()}

    def at(self, k, ts):
        if k not in self.ser:
            return None
        i = bisect.bisect_right(self.tijden[k], ts) - 1
        return self.ser[k][i][1] if i >= 0 else None

    def tw(self, k, a, b):
        """Tijdgewogen gemiddelde van entiteit k tussen a en b."""
        if k not in self.ser:
            return math.nan
        tot = acc = 0.0
        cur, last = self.at(k, a), a
        for x, v in self.ser[k]:
            if x <= a:
                continue
            if x > b:
                break
            if cur is not None:
                acc += cur * (x - last).total_seconds()
                tot += (x - last).total_seconds()
            cur, last = v, x
        if cur is not None:
            acc += cur * (b - last).total_seconds()
            tot += (b - last).total_seconds()
        return acc / tot if tot else math.nan

    def fractie(self, k, a, b, cond):
        if k not in self.ser:
            return math.nan
        tot = hit = 0.0
        cur, last = self.at(k, a), a
        for x, v in self.ser[k]:
            if x <= a:
                continue
            if x > b:
                break
            d = (x - last).total_seconds()
            tot += d
            hit += d if (cur is not None and cond(cur)) else 0
            cur, last = v, x
        d = (b - last).total_seconds()
        tot += d
        hit += d if (cur is not None and cond(cur)) else 0
        return hit / tot if tot else math.nan


def defrosts(r):
    out, on = [], None
    for x, v in r.ser.get('statusbyte_buitenunit_id3_byte_8', []):
        if v is None:
            continue
        b = int(v) & 1
        if b and on is None:
            on = x
        if not b and on is not None:
            out.append((on, x))
            on = None
    return out


def cop(t_aanvoer, t_buiten):
    """Zelfde model als chofu_wp_ff/regelaar.cpp ff_cop(): Carnot x 0,40."""
    ts, to = t_aanvoer + 273.15, t_buiten + 273.15
    if ts - to < 1:
        return 1.0
    return max(1.0, min(6.0, ts / (ts - to) * 0.40))


def main(path):
    r = Reeks(lees(path))
    df = defrosts(r)
    buiten_k = 'cresta_4802_temp' if 'cresta_4802_temp' in r.ser else 'buiten'
    print(f"Defrosts: {len(df)}")
    print('  ' + ', '.join(f"{a:%d %H:%M} ({int((b - a).total_seconds() // 60)} min)" for a, b in df))

    kamer = r.ser.get('anna_temperatuur') or r.ser['aanvoer']
    h = kamer[0][0].replace(minute=0, second=0, microsecond=0)
    end = kamer[-1][0]
    print(f"\n{'uur':>8} {'kamer':>5} {'sp':>4} {'k-sp':>5} {'buiten':>6} {'wpbui':>5} {'aanv':>5} {'doel':>5}"
          f" {'tekort':>6} {'stand':>5} {'st>=9':>5} {'P_el':>5} {'Q_th':>5} {'df':>2}")
    punten = []
    while h + D(hours=1) <= end:
        e = h + D(hours=1)
        nd = sum(1 for a, _ in df if h <= a < e)
        k, sp = r.tw('anna_temperatuur', h, e), r.tw('anna_setpoint', h, e)
        bu, wb = r.tw(buiten_k, h, e), r.tw('buiten', h, e)
        a, d = r.tw('aanvoer', h, e), r.tw('doel_aanvoer', h, e)
        p = r.tw('vermogen', h, e)
        q = p * cop(a, wb) if not (math.isnan(p) or math.isnan(a) or math.isnan(wb)) else math.nan
        print(f"{h:%d %H:00} {k:5.2f} {sp:4.1f} {k - sp:5.2f} {bu:6.1f} {wb:5.1f} {a:5.1f} {d:5.1f} {d - a:6.1f}"
              f" {r.tw('stand', h, e):5.1f} {r.fractie('stand', h, e, lambda v: v >= 9) * 100:4.0f}%"
              f" {p:5.0f} {q:5.0f} {nd:>2}")
        zon = 10 <= h.hour <= 15
        if not zon and not math.isnan(q) and not math.isnan(k - bu):
            punten.append((k - bu, q))
        h = e

    if punten:
        dtg = sum(x for x, _ in punten) / len(punten)
        qg = sum(q for _, q in punten) / len(punten)
        ua = qg / dtg
        print(f"\nWarmteverlies (uren zonder zon, incl. defrosts en interne warmte): {len(punten)} uur,"
              f" gem. {dtg:.1f} K binnen-buiten, gem. {qg:.0f} W -> {ua:.0f} W/K")
        for tb in (-5, -10):
            for tk in (18.5, 19.5):
                print(f"  nodig bij {tb} graden buiten, kamer {tk}: {ua * (tk - tb) / 1000:.1f} kW")


if __name__ == '__main__':
    main(sys.argv[1])
