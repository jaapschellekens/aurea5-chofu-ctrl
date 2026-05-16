#ifndef CHOFU_PROTOCOL_H
#define CHOFU_PROTOCOL_H
#include "globals.h"

uint8_t bereken_checksum(uint8_t *buf, uint8_t len);
void    stuur_stand_telegram();   // dispatcher: klassiek of JGC afhankelijk van parser_jgc
void    verwerk_telegram_0x91();
void    lees_warmtepomp_data();   // dispatcher: klassiek of JGC afhankelijk van parser_jgc
void    pas_sim_toe();
#endif // CHOFU_PROTOCOL_H
