#pragma once
#include "globals.h"

// ═══════════════════════════════════════════════════════════════
//  EEPROM ADRESSEN
// ═══════════════════════════════════════════════════════════════

#define EEPROM_MAGIC            0xAF  // v2.2: T_VORST default 4→2°C
#define ADDR_MAGIC              0
#define ADDR_SETPOINT           1
#define ADDR_KP                 5
#define ADDR_KI                 9
#define ADDR_KD                 13
#define ADDR_STOOKLIJN_GRENS    17
#define ADDR_STOOKLIJN_FACTOR   21
#define ADDR_T_VORST            25
#define ADDR_SUPPLY_MAX         29
#define ADDR_KOELING_MIN_BUITEN 33
#define ADDR_STOOKLIJN_UIT      37
#define ADDR_FF_UA_HOUSE        41
#define ADDR_FF_UA_EMITTER      45
#define ADDR_MODUS              49   // nieuw v2.1

void eeprom_init();
void eeprom_save();
void eeprom_load();
