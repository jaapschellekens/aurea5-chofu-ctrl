#ifndef CHOFU_PROTOCOL_H
#define CHOFU_PROTOCOL_H
#include "globals.h"

uint8_t bereken_checksum(uint8_t *buf, uint8_t len);
void    stuur_stand_telegram();
void    verwerk_telegram_0x91();
void    lees_warmtepomp_data();
void    pas_sim_toe();
#endif // CHOFU_PROTOCOL_H
