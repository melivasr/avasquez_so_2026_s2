// Armado y validación de tramas seriales con CRC-16.
//
// Formato (el detalle y los tipos de mensaje están en CONTRATO.md):
//   [A5 5A][tipo: 1 B][longitud: 2 B LE][carga útil][CRC-16: 2 B LE]
// El CRC cubre tipo + longitud + carga útil, no la marca de inicio.

#include "lib/protocol.h"

#include <string.h>

// CRC-16 CCITT-FALSE: polinomio 0x1021, valor inicial 0xFFFF, sin XOR final.
uint16_t protocol_crc16(const uint8_t *data, size_t length)
{
    if (data == nullptr && length != 0) {
        return 0;
    }

    uint16_t crc = 0xFFFF;

    // Cálculo bit a bit; es lento comparado con una tabla, pero las tramas
    // son pequeñas y así no se gasta RAM en la tabla.
    for (size_t i = 0; i < length; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;

        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 0x8000U) != 0) {
                crc = static_cast<uint16_t>((crc << 1) ^ 0x1021U);
            } else {
                crc = static_cast<uint16_t>(crc << 1);
            }
        }
    }

    return crc;
}

// Escribe en 'output' la trama completa. Devuelve false si los argumentos no
// son válidos o si no cabe en 'output_capacity'.
bool encode_frame(
    MessageType type,
    const uint8_t *payload,
    uint16_t payload_length,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length)
{
    if (output == nullptr || output_length == nullptr) {
        return false;
    }
    if (payload_length > FRAME_MAX_PAYLOAD) {
        return false;
    }
    // Una carga con largo > 0 necesita un puntero válido.
    if (payload_length > 0 && payload == nullptr) {
        return false;
    }

    const size_t total_length =
        FRAME_HEADER_SIZE + payload_length + FRAME_CRC_SIZE;

    if (output_capacity < total_length) {
        return false;
    }

    output[0] = FRAME_MAGIC_0;
    output[1] = FRAME_MAGIC_1;
    output[2] = static_cast<uint8_t>(type);
    output[3] = static_cast<uint8_t>(payload_length & 0xFFU);
    output[4] = static_cast<uint8_t>((payload_length >> 8) & 0xFFU);

    if (payload_length > 0) {
        memcpy(output + FRAME_HEADER_SIZE, payload, payload_length);
    }

    // El CRC empieza en el byte del tipo (posición 2): 1 de tipo + 2 de
    // longitud + la carga.
    const uint16_t crc = protocol_crc16(
        output + 2,
        3 + payload_length
    );

    const size_t crc_position = FRAME_HEADER_SIZE + payload_length;
    output[crc_position] = static_cast<uint8_t>(crc & 0xFFU);
    output[crc_position + 1] = static_cast<uint8_t>((crc >> 8) & 0xFFU);

    *output_length = total_length;
    return true;
}

// Valida una trama completa (marca, longitud, CRC) y la copia a 'output'.
// Devuelve false ante cualquier inconsistencia y no modifica 'output'.
bool decode_frame(
    const uint8_t *input,
    size_t input_length,
    Frame *output)
{
    if (input == nullptr || output == nullptr) {
        return false;
    }

    if (input_length < FRAME_HEADER_SIZE + FRAME_CRC_SIZE) {
        return false;
    }

    if (input[0] != FRAME_MAGIC_0 ||
        input[1] != FRAME_MAGIC_1) {
        return false;
    }

    const uint16_t payload_length =
        static_cast<uint16_t>(input[3]) |
        (static_cast<uint16_t>(input[4]) << 8);

    if (payload_length > FRAME_MAX_PAYLOAD) {
        return false;
    }

    const size_t expected_length =
        FRAME_HEADER_SIZE + payload_length + FRAME_CRC_SIZE;

    // El largo recibido debe coincidir exacto: ni truncada ni con bytes de más.
    if (input_length != expected_length) {
        return false;
    }

    const size_t crc_position = FRAME_HEADER_SIZE + payload_length;

    const uint16_t received_crc =
        static_cast<uint16_t>(input[crc_position]) |
        (static_cast<uint16_t>(input[crc_position + 1]) << 8);

    const uint16_t calculated_crc =
        protocol_crc16(input + 2, 3 + payload_length);

    if (received_crc != calculated_crc) {
        return false;
    }

    // Se arma en una variable local y se copia al final, para no dejar
    // 'output' a medias si algo falla antes.
    Frame candidate = {};
    candidate.type = static_cast<MessageType>(input[2]);
    candidate.length = payload_length;

    if (payload_length > 0) {
        memcpy(candidate.payload, input + FRAME_HEADER_SIZE, payload_length);
    }

    *output = candidate;
    return true;
}