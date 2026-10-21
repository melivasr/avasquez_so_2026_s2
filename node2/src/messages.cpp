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

// Escribe un número de 16 bits en 2 bytes, byte bajo primero (little-endian).
static void write_u16_le(uint8_t *bytes, uint16_t value){
    bytes[0] = value & 0xFF;          // bits 0-7
    bytes[1] = (value >> 8) & 0xFF;   // bits 8-15
}

// Un contador de 32 bits en un campo de 16: si no cabe queda en 0xFFFF
static uint16_t clamp_u16(uint32_t value){
    if (value > 0xFFFFU) {
        return 0xFFFFU;
    }
    return (uint16_t)value;
}

// Lee un número de 16 bits de 2 bytes, byte bajo primero (little-endian).
static uint16_t read_u16_le(const uint8_t *bytes){
    return (uint16_t)(bytes[0] | (bytes[1] << 8));
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

// START: [algoritmo (1)] [report_ticks (2)]
bool parse_start(const Frame *frame, Algorithm *algorithm, uint16_t *report_ticks){
    // tiene que haber frame y salidas, ser un START y la carga medir 3 bytes
    if (!frame || !algorithm || !report_ticks ||
        frame->type != (uint8_t)MessageType::Start ||
        frame->length != START_SIZE) {
        return false;
    }

    const uint8_t algorithm_code = frame->payload[0];

    // el algoritmo es RMS o EDF
    if (algorithm_code != (uint8_t)Algorithm::RMS &&
        algorithm_code != (uint8_t)Algorithm::EDF) {
        return false;
    }

    *algorithm = (Algorithm)algorithm_code;
    *report_ticks = read_u16_le(&frame->payload[1]);
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

// TICK_REPORT: [tick (4)] [misses (2)] [sporadic_ignored (2)] [ticks_lost (2)]
//              [events_dropped (2)] [bad_frames (2)] [max_busy_us (4)]
//              [max_wake_latency_us (4)]
void build_tick_report(Frame *frame, const TickReportData *data){
    frame->type = (uint8_t)MessageType::TickReport;
    frame->length = TICK_REPORT_SIZE;

    write_u32_le(&frame->payload[0], data->tick);
    write_u16_le(&frame->payload[4], clamp_u16(data->deadline_misses));
    write_u16_le(&frame->payload[6], clamp_u16(data->sporadic_ignored));
    write_u16_le(&frame->payload[8], clamp_u16(data->ticks_lost));
    write_u16_le(&frame->payload[10], clamp_u16(data->events_dropped));
    write_u16_le(&frame->payload[12], clamp_u16(data->bad_frames));
    write_u32_le(&frame->payload[14], data->max_busy_us);
    write_u32_le(&frame->payload[18], data->max_wake_latency_us);
}

bool tick_report_due(uint16_t period, uint32_t tick, uint32_t *next_report){
    // sin período, sin salida o todavía no llega al siguiente reporte
    if (period == 0 ||
        !next_report ||
        tick < *next_report) {
        return false;
    }

    // el siguiente múltiplo de period después de este tick
    *next_report = (tick / period + 1) * period;
    return true;
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
