#!/usr/bin/env python3
"""
adam_discover.py  —  Plugwise Adam API test & discover

Leest /core/domain_objects en print:
  - alle beschikbare <type> waarden (discover modus)
  - alle locaties met naam, temperatuur en setpoint
  - keteldata (intended_boiler_temperature etc.)

Credentials worden gelezen uit adam_api_test/config.h (zelfde bestand als
de Arduino sketch gebruikt).

Gebruik:
    python adam_discover.py            # discover + data
    python adam_discover.py --raw      # dump ruwe XML
    python adam_discover.py --types    # alleen type-lijst
"""

import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
import argparse

try:
    import requests
except ImportError:
    sys.exit("Installeer requests:  pip install requests")


# ── Credentials uit config.h lezen ───────────────────────────────────────────

def lees_config() -> dict:
    config_path = Path(__file__).parent / "config.h"
    if not config_path.exists():
        sys.exit(f"config.h niet gevonden: {config_path}\n"
                 f"Kopieer config.h.example naar config.h en vul in.")

    defines = {}
    for line in config_path.read_text().splitlines():
        m = re.match(r'#define\s+(\w+)\s+"([^"]*)"', line)
        if m:
            defines[m.group(1)] = m.group(2)

    for vereist in ("ADAM_IP", "ADAM_PASS"):
        if vereist not in defines:
            sys.exit(f"Ontbrekende define in config.h: {vereist}")

    return defines


# ── Adam XML ophalen ──────────────────────────────────────────────────────────

def haal_xml(config: dict) -> ET.Element:
    url  = f"http://{config['ADAM_IP']}/core/domain_objects"
    auth = ("smile", config["ADAM_PASS"])

    print(f"Verbinden met {url} ...", flush=True)
    try:
        r = requests.get(url, auth=auth, timeout=30)
        r.raise_for_status()
    except requests.exceptions.ConnectionError:
        sys.exit("Verbinding mislukt — controleer ADAM_IP")
    except requests.exceptions.HTTPError as e:
        sys.exit(f"HTTP fout: {e}  (wachtwoord onjuist?)")

    print(f"Response: {r.status_code}  grootte: {len(r.content)/1024:.1f} KB\n")
    return ET.fromstring(r.text)


# ── Discover: alle <type> waarden ────────────────────────────────────────────

def discover_types(root: ET.Element):
    types = sorted({el.text.strip() for el in root.iter("type") if el.text})
    print(f"── Alle <type> waarden ({len(types)}) ──────────────────")
    for t in types:
        print(f"  {t}")
    print()


# ── Locaties / zones ──────────────────────────────────────────────────────────

def print_locaties(root: ET.Element):
    print("── Locaties ────────────────────────────────────────")
    for loc in root.findall("location"):
        naam = loc.findtext("name", "?")
        typ  = loc.findtext("type", "?")

        # Temperatuur uit point_log
        t_kamer = sp_kamer = None
        for pl in loc.findall(".//point_log"):
            pl_type = pl.findtext("type", "")
            meas    = pl.findtext(".//measurement")
            if pl_type == "temperature"  and meas: t_kamer = float(meas)
            if pl_type == "thermostat"   and meas: sp_kamer = float(meas)

        # Setpoint ook uit actuator_functionalities (authoritatief)
        for tf in loc.findall(".//thermostat_functionality"):
            if tf.findtext("type") == "thermostat":
                sp_txt = tf.findtext("setpoint")
                if sp_txt: sp_kamer = float(sp_txt)

        if t_kamer is not None:
            print(f"  [{typ:15s}] {naam:20s}  "
                  f"temp={t_kamer:.1f} °C  SP={sp_kamer:.1f} °C" if sp_kamer else
                  f"  [{typ:15s}] {naam:20s}  temp={t_kamer:.1f} °C")
        else:
            print(f"  [{typ:15s}] {naam}  (geen temperatuursensor)")
    print()


# ── Keteldata ─────────────────────────────────────────────────────────────────

KETEL_TYPES = [
    "intended_boiler_temperature",
    "maximum_boiler_temperature",
    "boiler_temperature",
    "return_water_temperature",
    "central_heating_setpoint",
    "domestic_hot_water_setpoint",
    "modulation_level",
    "flame_state",
    "central_heater_water_pressure",
    "domestic_hot_water_state",
    "intended_central_heating_state",
]

def print_keteldata(root: ET.Element):
    print("── Ketel / heater_central ──────────────────────────")
    heater = root.find(".//appliance[type='heater_central']")
    if heater is None:
        print("  Geen heater_central appliance gevonden.")
        print()
        return

    naam = heater.findtext("name", "?")
    print(f"  Naam: {naam}")

    # point_log metingen
    gevonden = {}
    for pl in heater.findall(".//point_log"):
        pl_type = pl.findtext("type", "")
        meas    = pl.findtext(".//measurement")
        if meas:
            gevonden[pl_type] = meas

    # actuator setpoints
    for tf in heater.findall(".//thermostat_functionality"):
        tf_type = tf.findtext("type", "")
        sp      = tf.findtext("setpoint")
        if sp:
            gevonden[tf_type + " (setpoint)"] = sp

    # toggle states
    for tog in heater.findall(".//toggle_functionality"):
        tog_type = tog.findtext("type", "")
        state    = tog.findtext("state")
        if state:
            gevonden[tog_type] = state

    # Print bekende types eerst, daarna de rest
    geprint = set()
    for t in KETEL_TYPES:
        for key, val in gevonden.items():
            if key.startswith(t):
                print(f"  {key:45s}: {val}")
                geprint.add(key)

    overig = {k: v for k, v in gevonden.items() if k not in geprint}
    if overig:
        print("  --- overig ---")
        for k, v in overig.items():
            print(f"  {k:45s}: {v}")
    print()


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--raw",   action="store_true", help="dump ruwe XML")
    parser.add_argument("--types", action="store_true", help="alleen type-lijst")
    args = parser.parse_args()

    config = lees_config()
    root   = haal_xml(config)

    if args.raw:
        import xml.dom.minidom
        print(xml.dom.minidom.parseString(ET.tostring(root)).toprettyxml(indent="  "))
        return

    discover_types(root)

    if not args.types:
        print_locaties(root)
        print_keteldata(root)


if __name__ == "__main__":
    main()
