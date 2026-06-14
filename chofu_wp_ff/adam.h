#ifndef CHOFU_ADAM_H
#define CHOFU_ADAM_H
#include "globals.h"

// ═══════════════════════════════════════════════════════════════
//  PLUGWISE ADAM — directe lokale REST API (optioneel)
// ═══════════════════════════════════════════════════════════════
//
// Haalt setpoints en kamertemperatuur direct uit de Plugwise Adam i.p.v. via
// Home Assistant (MQTT). Alleen actief als bron==ADAM én modus==FF_WATER.
//
// Multizone: de Adam stuurt één aanvoer-setpoint voor het hele hydraulische
// systeem, gedreven door de zone met de grootste warmtevraag. We lezen daarom
// ALLE zones en kiezen de leidende zone (grootste SP−temp, of het Adam
// vraagsignaal indien beschikbaar) als bron voor t_kamer / t_kamer_gewenst.
// ff_UA_emitter mag alleen leren als de leidende zone stabiel is — zie
// adam_leer_emitter_ok (globals) en de leer-gate in regelaar.cpp.
//
// Configuratie wordt vanuit chofu_wp_ff.ino doorgegeven (config.h staat alleen
// in de .ino om dubbele symbolen te voorkomen).

// Poll-interval en tijdsbudget (zie docs/ADAM_INTEGRATIE.md §5)
#define ADAM_POLL_MS      30000UL   // elke 30 s pollen
#define ADAM_TIMEOUT_MS    4000UL   // hard tijdsbudget per fetch
#define ADAM_MAX_ZONES        8     // vast array — extra zones worden genegeerd

// Initialiseer met config uit de .ino. Zonder aanroep blijft de laag inert.
void adam_init(const char* ip, const char* pass,
               const char* const* zones, uint8_t zone_count);

// Aanroepen in loop(); regelt zelf interval, modus/bron- en mid-frame-guards.
void adam_poll();

// Status (voor web/MQTT)
bool        adam_beschikbaar();        // true als adam_init() is aangeroepen
const char* adam_status_str();         // "ok" / "fout" / "uit"
const char* adam_leider_naam();        // naam leidende zone, of "-"
uint32_t    adam_ms_sinds_ok();        // ms sinds laatste geslaagde fetch (0 = nooit)

#endif // CHOFU_ADAM_H
