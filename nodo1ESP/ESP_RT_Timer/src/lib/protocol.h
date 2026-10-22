#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

// Formato de trama:
// [A5 5A][tipo: 1 byte][longitud: 2 bytes LE]
// [carga útil: longitud bytes][CRC-16: 2 bytes LE]
constexpr uint8_t FRAME_MAGIC_0 = 0xA5;
constexpr uint8_t FRAME_MAGIC_1 = 0x5A;

constexpr size_t FRAME_HEADER_SIZE = 5; // Marca de Inicio + Tipo + Longitud = 2+1+2
constexpr size_t FRAME_CRC_SIZE = 2; // CRC-16 -> 2 bytes
constexpr size_t FRAME_MAX_PAYLOAD = 512; // Carga útil (Datos del mensaje)
constexpr size_t FRAME_MAX_SIZE =
    FRAME_HEADER_SIZE + FRAME_MAX_PAYLOAD + FRAME_CRC_SIZE;

// Mensajes mínimos
enum class MessageType : uint8_t {
    CalibrateReq = 0x01, // solicitud
    CalibrateResp = 0x02, // respuesta
    DeployTasks = 0x03, // despliegue
    Start = 0x04, // Inicio
    Event = 0x05,
    TickReport = 0x06,
    SporadicRelease = 0x07, // Disparo de tarea S
    Stop = 0x08,
    Ack = 0x09, // Acuse de recibo Acknowledge
    AddTask = 0x0A // Modo manual: agrega una tarea
};

// Trama compuesta por tipo de mensaje, tamaño y carga útil
struct Frame {
    MessageType type;
    uint16_t length;
    uint8_t payload[FRAME_MAX_PAYLOAD]; // carga de un tamaño de 0 a 512
};

uint16_t protocol_crc16(const uint8_t *data, size_t length);

bool encode_frame(
    MessageType type,
    const uint8_t *payload,
    uint16_t payload_length,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length
);

bool decode_frame(
    const uint8_t *input,
    size_t input_length,
    Frame *output
);

#endif
