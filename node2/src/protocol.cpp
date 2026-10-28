#include "protocol.h"
#include "crc16.h"
#include <string.h>

// Arma la trama: A5 5A | tipo | largo | carga | CRC
size_t encode_frame(const Frame *frame, uint8_t *out, size_t capacity){
    // no hay frame u output definido
    if (!frame || !out) {
        return 0;
    }
    // mensaje más largo que 512
    if (frame->length > MAX_PAYLOAD) {
        return 0;
    }

    // Tamaño real de la trama
    size_t size = FRAME_MIN_SIZE + frame->length;

    // no cabe en out
    if (size > capacity) {
        return 0;
    }

    // Marca de inicio
    out[0] = FRAME_START_1;
    out[1] = FRAME_START_2;

    // Tipo de mensaje
    out[2] = frame->type;

    // Largo de la carga en little-endian (byte bajo primero)
    out[3] = frame->length & 0xFF;
    out[4] = frame->length >> 8;

    // Copiar la carga útil
    memcpy(&out[5], frame->payload, frame->length);

    // CRC sobre tipo + largo + carga (empieza en out[2], no incluye A5 5A)
    uint16_t crc = crc16_compute(&out[2], 3 + frame->length);

    // CRC en los dos últimos bytes, también en little-endian
    out[size - 2] = crc & 0xFF;
    out[size - 1] = crc >> 8;

    return size;
}

// Lee una trama completa y valida marca, largo y CRC
bool decode_frame(const uint8_t *bytes, size_t size, Frame *frame){
    // no hay input u output definido
    if (!bytes || !frame) {
        return false;
    }
    // más corta que la trama mínima (marca + tipo + largo + CRC, sin carga)
    if (size < FRAME_MIN_SIZE) {
        return false;
    }
    // no empieza con la marca A5 5A
    if (bytes[0] != FRAME_START_1 ||
        bytes[1] != FRAME_START_2) {
        return false;
    }

    // Largo de la carga (little-endian)
    uint16_t length = bytes[3] | (bytes[4] << 8);

    // mensaje más largo que 512
    if (length > MAX_PAYLOAD) {
        return false;
    }

    // Tamaño que debería tener la trama según su largo
    size_t expected_size = FRAME_MIN_SIZE + length;

    // rechazar tanto tramas truncadas como bytes adicionales
    if (size != expected_size) {
        return false;
    }

    // CRC recibido: los dos últimos bytes
    uint16_t received_crc = bytes[size - 2] | (bytes[size - 1] << 8);

    // CRC calculado sobre tipo + largo + carga
    uint16_t calculated_crc = crc16_compute(&bytes[2], 3 + length);

    // la trama llegó dañada
    if (received_crc != calculated_crc) {
        return false;
    }

    // Trama válida: recién ahora se llena frame
    frame->type = bytes[2];
    frame->length = length;
    memcpy(frame->payload, &bytes[5], length);

    return true;
}
