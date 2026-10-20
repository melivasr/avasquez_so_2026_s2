// ================================================================
// IMPLEMENTA PASO DE MENSAJES CON CRC-16
// Ver DOCUMENTACION.md
// ================================================================


#include "protocol.h"

#include <string.h>

// Aplica CRC-16 CCCIT FALSE sin XOR final
uint16_t protocol_crc16(const uint8_t *data, size_t length)
{
    // Verificación
    if (data == nullptr && length != 0) {
        return 0;
    }

    // estado inicial
    uint16_t crc = 0xFFFF;

    // Algoritmo CRC-16
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

bool encode_frame(
    MessageType type,
    const uint8_t *payload,
    uint16_t payload_length,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length)
{
    // no hay output definido
    if (output == nullptr || output_length == nullptr) {
        return false;
    }
    // mensaje más largo que 512
    if (payload_length > FRAME_MAX_PAYLOAD) {
        return false;
    }
    // si no hay carga
    if (payload_length > 0 && payload == nullptr) {
        return false;
    }
    // Tamaño real
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

    // Copiar la memoria
    if (payload_length > 0) {
        memcpy(output + FRAME_HEADER_SIZE, payload, payload_length);
    }

    // CRC sobre tipo + longitud + carga útil.
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

    // Rechazar tanto tramas truncadas como bytes adicionales.
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

    Frame candidate = {};
    candidate.type = static_cast<MessageType>(input[2]);
    candidate.length = payload_length;

    if (payload_length > 0) {
        memcpy(candidate.payload, input + FRAME_HEADER_SIZE, payload_length);
    }

    *output = candidate;
    return true;
}
