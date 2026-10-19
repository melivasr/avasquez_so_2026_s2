// Trama: A5 5A | type | len_lo len_hi | payload (0..512) | crc_lo crc_hi
// El CRC cubre type + len + payload. len y CRC van en little-endian.
#pragma once

#include <stddef.h>
#include <stdint.h>

#define FRAME_START_1 0xA5
#define FRAME_START_2 0x5A
#define MAX_PAYLOAD 512U
#define FRAME_MIN_SIZE 7U                               // inicio(2) + tipo(1) + largo(2) + CRC(2)
#define FRAME_MAX_SIZE (FRAME_MIN_SIZE + MAX_PAYLOAD)

// Mismos codigos que el nodo ESP32
enum class MessageType : uint8_t {
    CalibrateReq = 0x01,
    CalibrateResp = 0x02,
    DeployTasks = 0x03,
    Start = 0x04,
    Event = 0x05,
    TickReport = 0x06,
    SporadicRelease = 0x07,
    Stop = 0x08,
    Ack = 0x09,
};

// Lo que lleva una trama. El inicio y el CRC los pone encode_frame
// y los revisa decode_frame.
struct Frame {
    uint8_t type;
    uint16_t length;
    uint8_t payload[MAX_PAYLOAD];
};

// Arma la trama en out. Devuelve cuantos bytes escribio, o 0 si no se pudo.
size_t encode_frame(const Frame *frame, uint8_t *out, size_t capacity);

// Lee una trama completa. Devuelve false si esta mal (y no toca frame).
bool decode_frame(const uint8_t *bytes, size_t size, Frame *frame);
