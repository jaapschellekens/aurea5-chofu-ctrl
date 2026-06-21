#ifndef CHOFU_EEPROM_H
#define CHOFU_EEPROM_H
#include "globals.h"

// ═══════════════════════════════════════════════════════════════
//  EEPROM ADRESSEN
// ═══════════════════════════════════════════════════════════════

#define EEPROM_MAGIC            0xB7  // v3.0: kamer_in_water toegevoegd
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
#define ADDR_WATER_SP_MIN       70   // nieuw v2.7: minimaal geldig water setpoint
#define ADDR_MAX_STAND          74   // nieuw v2.8: 1 byte — max compressorstand (niet-handmatig)
#define ADDR_SWW_SETPOINT       75   // nieuw v2.9: float — SWW laad-setpoint
#define ADDR_SWW_MAX_STAND      79   // nieuw v2.9: 1 byte — max stand tijdens SWW
#define ADDR_KAMER_IN_WATER     80   // nieuw v3.0: 1 byte — kamertemp gebruiken in water modi
#define ADDR_KOEL_DEADBAND      81   // float — koel-doodband (default-bij-ontbreken, geen magic-bump)

void eeprom_init();
void eeprom_save();
void eeprom_load();
#endif // CHOFU_EEPROM_H
