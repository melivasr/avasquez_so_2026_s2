// Carga útil de cada mensaje.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "event_queue.h"
#include "protocol.h"

#define CALIBRATE_REQ_SIZE 4U // tick_us
#define CALIBRATE_RESP_SIZE 4U // ut_per_tick 
#define ACK_SIZE 2U // tipo confirmado + status
#define TICK_REPORT_SIZE 9U // tick + state + running_task_id
#define EVENT_SIZE 13U // tick + event_type + task_id + value
#define START_SIZE 3U // algoritmo + report_ticks

#define FRAME_ERROR_CRC 1U    // CRC no coincide
#define FRAME_ERROR_LENGTH 2U // largo mayor a 512 o trama incompleta

// Códigos de estado del ACK
enum class StatusCode : uint8_t {
    Ok = 0,    // comando aceptado
    Error = 1, // carga inválida o comando no válido en el estado actual
    NotImplemented = 2, // tipo desconocido o que el nodo todavía no maneja
};

// Estado del nodo (se envía en TICK_REPORT)
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

// Llena frame con un TICK_REPORT (tick desde START, estado y tarea en ejecución)
void build_tick_report(Frame *frame, uint32_t tick, NodeState state, uint32_t running_task_id);

// Llena frame con un EVENT a partir de un evento de la cola
void build_event(Frame *frame, const NodeEvent *event);
