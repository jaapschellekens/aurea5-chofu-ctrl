#pragma once
#include "globals.h"

// ═══════════════════════════════════════════════════════════════
//  EEPROM ADRESSEN
// ═══════════════════════════════════════════════════════════════

#define EEPROM_MAGIC            0xB3  // v2.6: SUPPLY_MIN (condensatiebescherming koeling)
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
#define ADDR_MODUS              49   // 1 byte (uint8_t)
#define ADDR_KP_WATER           50   // nieuw v2.4: PID voor WATER/FF_WATER modus
#define ADDR_KI_WATER           54
#define ADDR_KD_WATER           58
#define ADDR_STOOKLIJN_AAN      62   // nieuw v2.5
#define ADDR_SUPPLY_MIN         66   // nieuw v2.6: condensatiebescherming koeling

void eeprom_init();
void eeprom_save();
void eeprom_load();
