// Trama: A5 5A | type | len_lo len_hi | payload (0..512) | crc_lo crc_hi
// El CRC cubre type + len + payload. len y CRC van en little-endian.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "../../common/rt_protocol.h"

#define FRAME_START_1 RT_MAGIC_0
#define FRAME_START_2 RT_MAGIC_1
#define MAX_PAYLOAD RT_MAX_PAYLOAD
#define FRAME_MIN_SIZE 7U                               // inicio(2) + tipo(1) + largo(2) + CRC(2)
#define FRAME_MAX_SIZE (FRAME_MIN_SIZE + MAX_PAYLOAD)

// Tipos de mensaje
enum class MessageType : uint8_t {
    CalibrateReq = RT_CAL_REQ,
    CalibrateResp = RT_CAL_RESP,
    DeployTasks = RT_DEPLOY,
    Start = RT_START,
    Event = RT_EVENT,
    TickReport = RT_TICK_REPORT,
    SporadicRelease = RT_SPORADIC,
    Stop = RT_STOP,
    Ack = RT_ACK,
    AddTask = RT_ADD_TASK,
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
