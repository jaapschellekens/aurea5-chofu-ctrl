#!/usr/bin/env python3
"""
wp_simulator.py — Hardware-in-the-loop WP simulatie

De Arduino draait echte firmware maar is NIET verbonden met de echte WP.
Dit script simuleert het huis en de warmtepomp en koppelt via MQTT:

  Arduino leest  ←  chofu/cmd/kamer       (gesimuleerde kamertemperatuur)
                 ←  chofu/sim/supply        (gesimuleerde aanvoertemperatuur)
                 ←  chofu/sim/return        (gesimuleerde retourtemperatuur)
                 ←  chofu/sim/outside       (gesimuleerde buitentemperatuur)
                 ←  chofu/cmd/kamer_setpoint           (gewenste kamertemperatuur)

  Arduino stuurt →  chofu/stand            (compressorstand 0-12)

  Script integreert HouseModel met ontvangen stand → nieuwe temperaturen → terug.

Installatie:
    pip install paho-mqtt

Gebruik:
    python wp_simulator.py
    python wp_simulator.py --host 192.168.1.8 --setpoint 20.5 --outside 3.0
    python wp_simulator.py --host 192.168.1.8 --speed 6   # 6× versneld
"""
import argparse
import math
import time
import sys
import paho.mqtt.client as mqtt

# Modi waarbij de Arduino op aanvoertemperatuur stuurt
WATER_MODI = {"water", "ff_water"}

# ── Warmtepomp elektrisch vermogen per stand ──────────────────────────────────
STAND_WATT = [0, 240, 420, 640, 850, 1050, 1250, 1450, 1550, 1650, 1700, 1750, 1800]

# ── Huismodel parameters (KGE-gekalibreerd nov25-feb26) ───────────────────────
UA_HOUSE   = 263.0    # W/K  warmteverlies gebouw
C_TH       = 12.5e6   # J/K  thermische massa gebouw
UA_EMITTER = 344.0    # W/K  warmteafgifte emitter (vloerverwarming/radiatoren)
C_WATER    = 293e3    # J/K  warmtecapaciteit water in circuit (≈ 70 liter)
COP_ETA    = 0.40     # fractie van Carnot COP (0.40 = realistisch lucht-WP)
G_FLOW     = 600.0    # W/K  m_dot × Cp afgifte-circuit


# ── HouseModel ────────────────────────────────────────────────────────────────
class HouseModel:
    def __init__(self, t_kamer=20.0, t_supply=35.0):
        self.t_kamer  = t_kamer
        self.t_supply = t_supply
        self.ua_eff   = 2.0 * G_FLOW * UA_EMITTER / (2.0 * G_FLOW + UA_EMITTER)

    def step(self, stand: int, t_outside: float, dt_s: float):
        """
        Integreer één tijdstap van dt_s seconden.

        Geeft (t_kamer, t_supply, t_return, cop, p_heat_W) terug.
        """
        # COP op basis van Carnot
        T_sup_K = self.t_supply + 273.15
        T_out_K = t_outside + 273.15
        if T_sup_K > T_out_K + 1.0:
            cop = min(COP_ETA * T_sup_K / (T_sup_K - T_out_K), 7.0)
        else:
            cop = 5.0  # WP kan altijd een klein beetje verwarmen

        p_elec  = float(STAND_WATT[max(0, min(stand, 12))])
        p_heat  = p_elec * cop                                             # thermisch naar water

        p_emit  = self.ua_eff * max(0.0, self.t_supply - self.t_kamer)    # water → kamer
        p_loss  = UA_HOUSE    * (self.t_kamer - t_outside)                 # kamer → buiten

        self.t_supply += (p_heat - p_emit) / C_WATER * dt_s
        self.t_kamer  += (p_emit - p_loss) / C_TH    * dt_s

        t_return = self.t_supply - p_emit / G_FLOW if G_FLOW > 0 else self.t_supply

        return self.t_kamer, self.t_supply, t_return, cop, p_heat


# ── Timing defaults (spiegelt firmware globals.cpp) ──────────────────────────
FF_MIN_OFF_MIN_DEFAULT   = 10    # minuten (600s)
AUTO_HYST_DOWN_MIN_DEFAULT = 5   # minuten
FF_RESTART_COAST_DEFAULT = 0.20  # °C
FF_LOOKAHEAD_MIN_DEFAULT = 5.0   # minuten (300s)
FF_THERMAL_MIN_OFF_MIN_DEFAULT = 1.0  # minuut (60s)


# ── MQTT simulator ────────────────────────────────────────────────────────────
class Simulator:
    def __init__(self, args):
        self.args     = args
        self.pfx      = args.prefix
        self.stand     = 0
        self.kamer_sp  = args.setpoint
        self.modus     = "auto"
        self.model     = HouseModel(t_kamer=args.setpoint, t_supply=35.0)
        self.t_outside = args.outside

        self.client = mqtt.Client(client_id="wp_simulator")
        self.client.on_connect    = self._on_connect
        self.client.on_message    = self._on_message
        self.client.on_disconnect = self._on_disconnect

        if args.user:
            self.client.username_pw_set(args.user, args.password)

    def _on_connect(self, client, userdata, flags, rc):
        if rc != 0:
            print(f"MQTT verbinding mislukt (rc={rc})")
            sys.exit(1)
        print(f"MQTT verbonden met {self.args.host}:{self.args.port}")
        client.subscribe(f"{self.pfx}/stand")
        client.subscribe(f"{self.pfx}/modus")
        client.subscribe(f"{self.pfx}/cmd/kamer_setpoint")
        print(f"Subscribed: {self.pfx}/stand, {self.pfx}/modus, {self.pfx}/cmd/kamer_setpoint")
        client.publish(f"{self.pfx}/cmd/sim", "1", retain=False)
        print("Simulatie ingeschakeld op Arduino")
        if self.args.modus:
            client.publish(f"{self.pfx}/cmd/modus", self.args.modus, retain=True)
            print(f"Modus ingesteld: {self.args.modus}")
        self._publish_all()  # stuur direct begintoestand

    def _on_message(self, client, userdata, msg):
        topic   = msg.topic
        payload = msg.payload.decode().strip()
        try:
            if topic == f"{self.pfx}/stand":
                self.stand = int(float(payload))
            elif topic == f"{self.pfx}/modus":
                self.modus = payload
            elif topic == f"{self.pfx}/cmd/kamer_setpoint":
                self.kamer_sp = float(payload)
        except ValueError:
            pass

    def _on_disconnect(self, client, userdata, rc):
        if rc != 0:
            print(f"MQTT verbinding verbroken (rc={rc}), herverbinden...")

    def _pub(self, topic, value, retain=False):
        if isinstance(value, float):
            payload = f"{value:.1f}"
        else:
            payload = str(value)
        self.client.publish(topic, payload, retain=retain)

    def _set_arduino_timing(self, speed: float):
        """Schaal timing-parameters met speed zodat gesimuleerde tijden kloppen.

        Bij speed=1 worden de firmware-defaults hersteld.
        Bij speed>1 worden timers verkleind zodat 1 gesimuleerde minuut
        ook 1 echte minuut aan hysteresis oplevert.
        """
        ff_min_off   = max(0.0, FF_MIN_OFF_MIN_DEFAULT   / speed)
        hyst_down    = max(0.0, AUTO_HYST_DOWN_MIN_DEFAULT / speed)
        ff_lookahead = max(0.0, FF_LOOKAHEAD_MIN_DEFAULT  / speed)
        ff_thermal   = max(0.0, FF_THERMAL_MIN_OFF_MIN_DEFAULT / speed)
        restart_coast = FF_RESTART_COAST_DEFAULT

        self.client.publish(f"{self.pfx}/cmd/ff_min_off",         f"{ff_min_off:.2f}")
        self.client.publish(f"{self.pfx}/cmd/auto_hyst_down",     f"{hyst_down:.2f}")
        self.client.publish(f"{self.pfx}/cmd/ff_lookahead",       f"{ff_lookahead:.2f}")
        self.client.publish(f"{self.pfx}/cmd/ff_thermal_min_off", f"{ff_thermal:.2f}")
        self.client.publish(f"{self.pfx}/cmd/ff_restart_coast",   f"{restart_coast:.2f}")

        if speed != 1.0:
            print(f"  Timing geschaald (speed {speed}×):")
            print(f"    ff_min_off    : {FF_MIN_OFF_MIN_DEFAULT} min → {ff_min_off:.2f} min real ({FF_MIN_OFF_MIN_DEFAULT:.0f} min sim)")
            print(f"    auto_hyst_down: {AUTO_HYST_DOWN_MIN_DEFAULT} min → {hyst_down:.2f} min real ({AUTO_HYST_DOWN_MIN_DEFAULT:.0f} min sim)")
            print(f"    ff_lookahead  : {FF_LOOKAHEAD_MIN_DEFAULT} min → {ff_lookahead:.2f} min real ({FF_LOOKAHEAD_MIN_DEFAULT:.0f} min sim)")

    def _restore_arduino_timing(self):
        """Herstel firmware-defaults na afloop van de simulatie."""
        self.client.publish(f"{self.pfx}/cmd/ff_min_off",         str(FF_MIN_OFF_MIN_DEFAULT))
        self.client.publish(f"{self.pfx}/cmd/auto_hyst_down",     str(AUTO_HYST_DOWN_MIN_DEFAULT))
        self.client.publish(f"{self.pfx}/cmd/ff_lookahead",       str(FF_LOOKAHEAD_MIN_DEFAULT))
        self.client.publish(f"{self.pfx}/cmd/ff_thermal_min_off", str(FF_THERMAL_MIN_OFF_MIN_DEFAULT))
        self.client.publish(f"{self.pfx}/cmd/ff_restart_coast",   str(FF_RESTART_COAST_DEFAULT))
        print("Arduino timing hersteld naar firmware defaults.")

    def _water_setpoint(self):
        """
        Bereken de benodigde aanvoertemperatuur voor het kamer setpoint
        op basis van de steady-state warmtebalans:
          UA_emitter_eff × (T_supply − T_kamer_sp) = UA_house × (T_kamer_sp − T_buiten)
          → T_supply = T_kamer_sp + (UA_house / UA_emitter_eff) × (T_kamer_sp − T_buiten)
        """
        t_sp = self.kamer_sp
        delta = max(0.0, t_sp - self.t_outside)
        return t_sp + (UA_HOUSE / self.model.ua_eff) * delta

    def _publish_all(self):
        t_water_sp = self._water_setpoint()
        self._pub(f"{self.pfx}/cmd/kamer",        self.model.t_kamer,   retain=True)
        self._pub(f"{self.pfx}/cmd/kamer_setpoint",           self.kamer_sp,        retain=True)
        self._pub(f"{self.pfx}/sim/supply",        self.model.t_supply,  retain=True)
        self._pub(f"{self.pfx}/sim/return",        self.model.t_supply - 3.0, retain=True)
        self._pub(f"{self.pfx}/sim/outside",       self.t_outside,       retain=True)
        self._pub(f"{self.pfx}/sim/kamer",         self.model.t_kamer,   retain=True)
        self._pub(f"{self.pfx}/sim/kamer_gewenst", self.kamer_sp,        retain=True)
        self._pub(f"{self.pfx}/sim/water_setpoint", t_water_sp,          retain=True)

    def run(self):
        self.client.connect(self.args.host, self.args.port, keepalive=60)
        self.client.loop_start()

        dt_real = self.args.dt                   # wachttijd per stap in seconden
        dt_sim  = dt_real * self.args.speed       # gesimuleerde tijd per stap

        # Wacht even zodat MQTT verbinding opgezet is en Arduino subscriptions actief zijn
        time.sleep(1.0)
        self._set_arduino_timing(self.args.speed)

        print(f"\nSimulatie gestart:")
        print(f"  T_kamer init   = {self.model.t_kamer:.1f}°C")
        print(f"  T_supply init  = {self.model.t_supply:.1f}°C")
        print(f"  T_buiten       = {self.t_outside:.1f}°C")
        print(f"  Setpoint kamer = {self.kamer_sp:.1f}°C")
        print(f"  Tijdstap       = {dt_sim:.0f}s sim / {dt_real:.0f}s real  (speed {self.args.speed}×)")
        print(f"{'─'*88}")
        water_modus = self.args.modus in WATER_MODI
        if water_modus:
            print(f"{'tijd':>8}  {'modus':>8}  {'stand':>5}  {'T_supply':>9}  "
                  f"{'T_water_sp':>10}  {'afwijk':>7}  {'T_kamer':>8}  {'T_buiten':>9}  {'kW':>5}")
            print(f"{'─'*82}")
        else:
            print(f"{'tijd':>8}  {'modus':>8}  {'stand':>5}  {'T_kamer':>8}  "
                  f"{'T_supply':>9}  {'T_water_sp':>10}  {'T_buiten':>9}  {'COP':>5}  {'kW':>5}")
            print(f"{'─'*88}")

        t_sim_s = 0
        try:
            while True:
                t_k, t_s, t_r, cop, p_heat = self.model.step(
                    self.stand, self.t_outside, dt_sim
                )

                # Buitentemperatuur: sinusoïdaal dagprofiel (optioneel)
                if self.args.daily_swing > 0:
                    hour = (t_sim_s / 3600.0) % 24
                    self.t_outside = (self.args.outside
                                      + self.args.daily_swing * math.sin(
                                          math.pi * (hour - 6) / 12))

                t_water_sp = self._water_setpoint()

                # Publiceer naar Arduino (retain=True zodat waarden na reconnect bewaard blijven)
                self._pub(f"{self.pfx}/cmd/kamer",         t_k,              retain=True)
                self._pub(f"{self.pfx}/sim/supply",         t_s,              retain=True)
                self._pub(f"{self.pfx}/sim/return",         t_r,              retain=True)
                self._pub(f"{self.pfx}/sim/outside",        self.t_outside,   retain=True)
                self._pub(f"{self.pfx}/sim/kamer",          t_k,              retain=True)
                self._pub(f"{self.pfx}/sim/water_setpoint", t_water_sp,       retain=True)

                # Console
                uren = int(t_sim_s) // 3600
                minu = (int(t_sim_s) % 3600) // 60
                if self.modus in WATER_MODI:
                    afwijk = t_s - t_water_sp
                    print(f"{uren:4d}:{minu:02d}  "
                          f"{self.modus:>8}  "
                          f"{self.stand:5d}  "
                          f"{t_s:9.2f}°C  "
                          f"{t_water_sp:10.2f}°C  "
                          f"{afwijk:+7.2f}°C  "
                          f"{t_k:8.2f}°C  "
                          f"{self.t_outside:9.1f}°C  "
                          f"{p_heat/1000:5.2f}")
                else:
                    print(f"{uren:4d}:{minu:02d}  "
                          f"{self.modus:>8}  "
                          f"{self.stand:5d}  "
                          f"{t_k:8.2f}°C  "
                          f"{t_s:9.2f}°C  "
                          f"{t_water_sp:10.2f}°C  "
                          f"{self.t_outside:9.1f}°C  "
                          f"{cop:5.2f}  "
                          f"{p_heat/1000:5.2f}")

                t_sim_s += dt_sim
                time.sleep(dt_real)

        except KeyboardInterrupt:
            print("\nSimulatie gestopt.")
        finally:
            self.client.publish(f"{self.pfx}/cmd/sim", "0", retain=False)
            print("Simulatie uitgeschakeld op Arduino.")
            self._restore_arduino_timing()
            time.sleep(0.5)  # geef MQTT tijd om laatste berichten te versturen
            self.client.loop_stop()
            self.client.disconnect()


# ── Argumenten ────────────────────────────────────────────────────────────────
def parse_args():
    p = argparse.ArgumentParser(description="Hardware-in-the-loop WP simulatie")
    p.add_argument("--host",        default="localhost",  help="MQTT broker IP")
    p.add_argument("--port",        default=1883, type=int)
    p.add_argument("--user",        default="",   help="MQTT gebruikersnaam")
    p.add_argument("--password",    default="",   help="MQTT wachtwoord")
    p.add_argument("--prefix",      default="chofu", help="MQTT topic-prefix (moet matchen met MQTT_PREFIX in types.h; bv. chofu_test)")
    p.add_argument("--setpoint",    default=20.0, type=float, help="Kamer setpoint (°C)")
    p.add_argument("--outside",     default=5.0,  type=float, help="Buitentemperatuur (°C)")
    p.add_argument("--daily-swing", default=0.0,  type=float,
                   help="Amplitude dagelijkse buitentemp variatie (°C), 0 = vast")
    p.add_argument("--dt",          default=10.0, type=float,
                   help="Real-time tijdstap in seconden (default 10)")
    p.add_argument("--speed",       default=1.0,  type=float,
                   help="Tijdversnelling (bijv. 6 = 1 minuut sim per 10s real)")
    p.add_argument("--modus",       default="",   help="Stuur modus naar Arduino bij start: auto / ff_auto / water / ff_water / handmatig")
    p.add_argument("--t-kamer",     default=20.0, type=float, help="Begin kamertemperatuur")
    p.add_argument("--t-supply",    default=35.0, type=float, help="Begin aanvoertemperatuur")
    return p.parse_args()


if __name__ == "__main__":
    args = parse_args()
    sim = Simulator(args)
    sim.model.t_kamer = args.t_kamer

    # Bereken passende begintemperatuur voor de supply als de gebruiker
    # geen expliciete waarde opgaf (default 35.0).
    # Water/ff_water: start op het berekende setpoint zodat de Arduino
    # niet direct in de shutdown-zone start en een uur wacht op afkoeling.
    # Auto/ff_auto: start op de stooklijn waarde.
    if args.t_supply != 35.0:
        sim.model.t_supply = args.t_supply
    elif args.modus in WATER_MODI:
        sim.model.t_supply = max(25.0, sim._water_setpoint())
    else:
        stooklijn_grens  = 15.0
        stooklijn_factor = 0.68
        stooklijn_sp     = 28.0
        if args.outside < stooklijn_grens:
            sim.model.t_supply = min(45.0, stooklijn_sp +
                                     (stooklijn_grens - args.outside) * stooklijn_factor)
        else:
            sim.model.t_supply = stooklijn_sp
    sim.run()
