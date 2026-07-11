#ifndef CHOFU_ADAM_H
#define CHOFU_ADAM_H
#include "globals.h"

// ═══════════════════════════════════════════════════════════════
//  PLUGWISE ADAM API (optioneel) — zie docs/ADAM_INTEGRATIE_PLAN.md
//
//  Niet-blokkerende poll van /core/domain_objects:
//   - intended_boiler_temperature → t_water_gewenst (water/ff_water)
//       SP 0  = geen vraag, SP 10 = koelvraag (Anna), SP ≥ 16 = verwarmen
//   - zonetemperaturen → gepubliceerd op MQTT; ADAM_ZONE → t_kamer
//   - bron "multi": t_kamer/t_kamer_gewenst van de zone met de
//     grootste warmtevraag (setpoint − temp), met 0.3°C wissel-hysterese
//
//  Activeren: ADAM_IP/ADAM_PASS in config.h + chofu/cmd/adam_bron
// ═══════════════════════════════════════════════════════════════

enum class AdamBron : uint8_t { UIT = 0, WATER = 1, WATER_KAMER = 2, MULTI = 3 };

extern AdamBron adam_bron;      // runtime bronkeuze (EEPROM)
extern float    ADAM_KOEL_SP;   // watersetpoint bij Adam-koelvraag (EEPROM, 16–25°C)

// Zonetabel (gevuld per poll, gepubliceerd in stuur_data)
#define ADAM_MAX_ZONES 8
struct AdamZone { char naam[21]; float temp; float sp; };  // sp = zone-thermostaatsetpoint (NAN indien geen)
extern AdamZone adam_zones[ADAM_MAX_ZONES];
extern uint8_t  adam_zone_count;

// In setup() aanroepen (credentials komen uit config.h, die alleen de .ino ziet):
//   #if defined(ADAM_IP)
//     adam_config(ADAM_IP, ADAM_PASS, ADAM_ZONE);
//   #endif
void        adam_config(const char* ip, const char* pass, const char* zone);

void        adam_poll();       // elke loop()-doorgang aanroepen; doet zelf timing
void        adam_pas_toe();    // resultaat toepassen op regeling (vóór pas_sim_toe)
bool        adam_actief();     // bron != UIT en ADAM_IP geconfigureerd
bool        adam_geldig();     // laatste poll ok en < 90s oud
float       adam_water_sp();   // NAN indien ongeldig
float       adam_zone_temp();  // temperatuur van ADAM_ZONE, NAN indien ongeldig
const char* adam_bron_str();   // "uit" / "water" / "water+kamer" / "multi"
const char* adam_actieve_zone();// actieve zone in multi-modus ("" indien geen)
const char* adam_status_str(); // "uit" / "ok" / "fout"
const char* adam_fout_str();   // reden laatste mislukte poll ("" = geen)

#endif // CHOFU_ADAM_H
