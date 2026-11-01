#ifndef CHOFU_PROTOCOL_H
#define CHOFU_PROTOCOL_H
#include "globals.h"

void stuur_stand_telegram();   // JGC multi-frame TX
void lees_warmtepomp_data();   // JGC multi-frame RX + TX timing
void pas_sim_toe();
String wp_diag_json();        // diagnosewaarden (verdamper, ventilator, EEV, statusbytes)
#endif // CHOFU_PROTOCOL_H
