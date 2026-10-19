#pragma once

#include <stddef.h>
#include <stdint.h>

#define CRC16_INIT 0xFFFFU
#define CRC16_POLY 0x1021U

// Agrega un byte a un CRC parcial (permite calcularlo byte a byte en el parser)
uint16_t crc16_update(uint16_t crc, uint8_t byte);

// CRC de un bloque completo, partiendo de CRC16_INIT
uint16_t crc16_compute(const uint8_t *data, size_t length);
