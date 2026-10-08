#pragma once
#include <stdint.h>

void xcan_protocol_init( void );
void xcan_protocol_poll( void );
/* main.c: reset; on the F072 the next boot enters the ST DFU bootloader */
void xcan_reset_to_bootloader( void );
