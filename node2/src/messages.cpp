//Carga util de cada mensaje
#include "messages.h"

// Escribe un número de 32 bits en 4 bytes, byte bajo primero (little-endian).
// Ejemplo: 10000 = 0x00002710 -> 10 27 00 00

static void write_u32_le(uint8_t *bytes, uint32_t value){
    bytes[0] = value & 0xFF;          // bits 0-7
    bytes[1] = (value >> 8) & 0xFF;   // bits 8-15
    bytes[2] = (value >> 16) & 0xFF;  // bits 16-23
    bytes[3] = (value >> 24) & 0xFF;  // bits 24-31
}

// Lee un número de 32 bits de 4 bytes, byte bajo primero (little-endian).
static uint32_t read_u32_le(const uint8_t *bytes){
    uint32_t value = bytes[0];  // bits 0-7
    value |= (uint32_t)bytes[1] << 8;   // bits 8-15
    value |= (uint32_t)bytes[2] << 16;  // bits 16-23
    value |= (uint32_t)bytes[3] << 24;  // bits 24-31
    return value;
}

// CALIBRATE_REQ: [tick_us (4)]
bool parse_calibrate_req(const Frame *frame, uint32_t *tick_us){
    // sin frame o salida, no es un CALIBRATE_REQ o la carga no mide 4 bytes
    if (!frame || !tick_us ||
        frame->type != (uint8_t)MessageType::CalibrateReq ||
        frame->length != CALIBRATE_REQ_SIZE) {
        return false;
    }

    *tick_us = read_u32_le(&frame->payload[0]);
    return true;
}

// CALIBRATE_RESP: [ut_per_tick (4)]
void build_calibrate_resp(Frame *frame, uint32_t ut_per_tick){
    frame->type = (uint8_t)MessageType::CalibrateResp;
    frame->length = CALIBRATE_RESP_SIZE;

    write_u32_le(&frame->payload[0], ut_per_tick);
}

// ACK: [tipo confirmado (1)] [status (1)]
void build_ack(Frame *frame, uint8_t acked_type, StatusCode status){
    frame->type = (uint8_t)MessageType::Ack;
    frame->length = ACK_SIZE;

    frame->payload[0] = acked_type;
    frame->payload[1] = (uint8_t)status;
}

// TICK_REPORT: [tick (4)] [state (1)] [running_task_id (4)]
void build_tick_report(Frame *frame, uint32_t tick, NodeState state, uint32_t running_task_id){
    frame->type = (uint8_t)MessageType::TickReport;
    frame->length = TICK_REPORT_SIZE;

    write_u32_le(&frame->payload[0], tick);
    frame->payload[4] = (uint8_t)state;
    write_u32_le(&frame->payload[5], running_task_id);
}

// EVENT: [tick (4)] [event_type (1)] [task_id (4)] [value (4)]
void build_event(Frame *frame, const NodeEvent *event){
    frame->type = (uint8_t)MessageType::Event;
    frame->length = EVENT_SIZE;

    write_u32_le(&frame->payload[0], event->tick);
    frame->payload[4] = (uint8_t)event->type;
    write_u32_le(&frame->payload[5], event->task_id);
    write_u32_le(&frame->payload[9], event->value);
}
