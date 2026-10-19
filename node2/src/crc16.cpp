#include "crc16.h"

// Procesa un byte: entra por la parte alta del registro (MSB primero)
uint16_t crc16_update(uint16_t crc, uint8_t byte)
{
    crc ^= static_cast<uint16_t>(byte) << 8;

    for (int bit = 0; bit < 8; ++bit) {
        if ((crc & 0x8000U) != 0) {
            crc = static_cast<uint16_t>((crc << 1) ^ CRC16_POLY);
        } else {
            crc = static_cast<uint16_t>(crc << 1);
        }
    }

    return crc;
}

// Aplica CRC-16 CCITT 
uint16_t crc16_compute(const uint8_t *data, size_t length)
{
    // Verificación
    if (data == nullptr) {
        return CRC16_INIT;
    }

    // estado inicial
    uint16_t crc = CRC16_INIT;

    for (size_t i = 0; i < length; ++i) {
        crc = crc16_update(crc, data[i]);
    }

    return crc;
}
