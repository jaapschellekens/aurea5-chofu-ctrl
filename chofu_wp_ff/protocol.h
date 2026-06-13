#ifndef CHOFU_PROTOCOL_H
#define CHOFU_PROTOCOL_H
#include "globals.h"

void stuur_stand_telegram();   // JGC multi-frame TX
void lees_warmtepomp_data();   // JGC multi-frame RX + TX timing
void pas_sim_toe();
#endif // CHOFU_PROTOCOL_H
