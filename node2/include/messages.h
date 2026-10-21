// Carga útil de cada mensaje.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "event_queue.h"
#include "protocol.h"

#define CALIBRATE_REQ_SIZE 4U // tick_us
#define CALIBRATE_RESP_SIZE 4U // ut_per_tick 
#define ACK_SIZE 2U // tipo confirmado + status
#define TICK_REPORT_SIZE 22U // tick + 5 contadores de 2 bytes + 2 tiempos de 4 bytes
#define EVENT_SIZE 13U // tick + event_type + task_id + value
#define START_SIZE 3U // algoritmo + report_ticks

// Códigos de estado del ACK 
enum class StatusCode : uint8_t {
    Ok = 0,  // comando aceptado
    Error = 1,  // carga inválida o rechazada 
    NotImplemented = 2, // tipo desconocido o que el nodo todavía no maneja
    BadState = 3,       // comando no válido en el estado actual (NODE_BAD_STATE)
};

// Estado del nodo
enum class NodeState : uint8_t {
    Uncalibrated = 0,
    Calibrated = 1,
    Configured = 2,
    Running = 3,
    Stopped = 4,
    Error = 5,
};

// Algoritmo de calendarización 
enum class Algorithm : uint8_t {
    RMS = 0,
    EDF = 1,
};

// Lee un START: algoritmo y cada cuántos ticks se manda TICK_REPORT
// (0 = no se manda). 
bool parse_start(const Frame *frame, Algorithm *algorithm, uint16_t *report_ticks);

// Lee tick_us de un CALIBRATE_REQ.
// Devuelve false si no es CALIBRATE_REQ o la carga no mide 4 bytes.
bool parse_calibrate_req(const Frame *frame, uint32_t *tick_us);

// Llena frame con un CALIBRATE_RESP (los errores de calibración van en un ACK)
void build_calibrate_resp(Frame *frame, uint32_t ut_per_tick);

// Llena frame con un ACK que confirma el mensaje de tipo acked_type
void build_ack(Frame *frame, uint8_t acked_type, StatusCode status);

// Lo que lleva un TICK_REPORT. Los contadores
// se cuentan desde START; en la trama van topados a 0xFFFF.
struct TickReportData {
    uint32_t tick; // tick recién procesado
    uint32_t deadline_misses; // trabajos abortados por plazo vencido
    uint32_t sporadic_ignored; // pulsaciones antes del intervalo mínimo
    uint32_t ticks_lost; // ticks que no se alcanzaron a procesar
    uint32_t events_dropped; // eventos descartados por cola llena
    uint32_t bad_frames; // tramas descartadas (CRC, largo o incompletas)
    uint32_t max_busy_us; // lo más que tardó un tick
    uint32_t max_wake_latency_us; // lo más que tardó en atenderse un tick
};

// Llena frame con un TICK_REPORT
void build_tick_report(Frame *frame, const TickReportData *data);

// true si en el tick toca mandar TICK_REPORT, y deja en next_report el
// siguiente múltiplo de period. Con period 0 nunca toca.
bool tick_report_due(uint16_t period, uint32_t tick, uint32_t *next_report);

// Llena frame con un EVENT a partir de un evento de la cola
void build_event(Frame *frame, const NodeEvent *event);
