#ifndef CHOFU_PROTOCOL_H
#define CHOFU_PROTOCOL_H
#include "globals.h"

void stuur_stand_telegram();   // JGC multi-frame TX
void lees_warmtepomp_data();   // JGC multi-frame RX + TX timing
void pas_sim_toe();
bool jgc_ontvangend();         // true = parser zit mid-frame (blokkeer trage taken)
#endif // CHOFU_PROTOCOL_H
