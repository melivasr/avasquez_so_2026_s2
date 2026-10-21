#include <Arduino.h>

#include "node.h"
#include "calibration.h"
#include "messages.h"
#include "scheduler/scheduler_edf.h"
#include "scheduler/scheduler_rms.h"
#include "task_parser.h"
#include "tick_timer.h"
#include "usb_communication.h"
extern "C" {
#include "ut.h"
}

// Cada cuántos ticks cambia el LED (50 x 10 ms = medio segundo)
#define LED_EVERY_TICKS 50U

// Rango aceptado para el tick: 1 ms a 1 s
#define TICK_MIN_US 1000U
#define TICK_MAX_US 1000000U

// Estado del nodo y tick configurado
static NodeState state = NodeState::Uncalibrated;
static uint32_t tick_us = TICK_DEFAULT_US;

// UT por tick medidas en la última calibración (s_n)
static uint32_t ut_per_tick = 0;

// Ticks desde START
static uint32_t tick_count = 0;

// Cada cuántos ticks se envía TICK_REPORT (llega en START; 0 = no se envía)
// y en qué tick toca el próximo
static uint16_t report_ticks = 0;
static uint32_t next_report = 0;

// Trabajos abortados por plazo vencido desde START (va en TICK_REPORT)
static uint32_t deadline_misses = 0;

// Tareas desplegadas por el host
static Task tasks[MAX_TASKS];
static size_t task_count = 0;

// Calendarizadores. En START se elige uno según el algoritmo que trae
static SchedulerRms scheduler_rms;
static SchedulerEdf scheduler_edf;
static Scheduler *scheduler = nullptr;

// Tarea que corrió en el último tick (NO_TASK si ninguna)
static uint32_t running_task = NO_TASK;

static bool led = false;

// Responde un ACK al mensaje recibido
static void send_ack(const Frame *frame, StatusCode status){
    static Frame reply;

    build_ack(&reply, frame->type, status);
    usb_send(&reply);
}

// CALIBRATE_REQ: mide cuántas UT caben en un tick
static void handle_calibrate_req(const Frame *frame){
    static Frame reply;
    uint32_t requested_us = 0;

    // carga inválida o tick fuera de rango: el error va en un ACK
    if (!parse_calibrate_req(frame, &requested_us) ||
        requested_us < TICK_MIN_US ||
        requested_us > TICK_MAX_US) {
        send_ack(frame, StatusCode::Error);
        return;
    }

    // no se calibra con el calendarizador corriendo
    if (state == NodeState::Running) {
        send_ack(frame, StatusCode::BadState);
        return;
    }

    const uint32_t measured = calibration_measure(requested_us);

    // no cupo ni una UT en el tick: se rechaza y queda la calibración anterior
    if (measured == 0) {
        send_ack(frame, StatusCode::Error);
        return;
    }

    // se guarda el tick para usarlo en START
    ut_per_tick = measured;
    tick_us = requested_us;
    state = NodeState::Calibrated;

    build_calibrate_resp(&reply, ut_per_tick);
    usb_send(&reply);
}

// DEPLOY_TASKS: guarda las tareas que manda el host
static void handle_deploy_tasks(const Frame *frame){
    // se lee en una tabla aparte: si alguna línea está mala,
    // las tareas anteriores quedan como estaban
    static Task pending[MAX_TASKS];
    size_t count = 0;

    // no se cambian las tareas con el calendarizador corriendo
    if (state == NodeState::Running) {
        send_ack(frame, StatusCode::BadState);
        return;
    }

    // alguna tarea mal escrita (se puede desplegar antes de calibrar)
    if (!parse_task_list((const char *)frame->payload, frame->length, pending, MAX_TASKS, &count)) {
        send_ack(frame, StatusCode::Error);
        return;
    }

    // todas las tareas son válidas: reemplazan a las anteriores
    for (size_t i = 0; i < count; i++) {
        tasks[i] = pending[i];
    }
    task_count = count;

    // sin calibrar sigue faltando la calibración para START
    if (state != NodeState::Uncalibrated) {
        state = NodeState::Configured;
    }

    send_ack(frame, StatusCode::Ok);
}

// START: tick 0 y arranca el timer
static void handle_start(const Frame *frame){
    Algorithm algorithm;
    uint16_t requested_report_ticks = 0;

    // carga inválida
    if (!parse_start(frame, &algorithm, &requested_report_ticks)) {
        send_ack(frame, StatusCode::Error);
        return;
    }

    // falta calibrar o ya está corriendo. Sin tareas desplegadas
    // sí arranca y queda esperando
    if (state == NodeState::Uncalibrated ||
        state == NodeState::Running) {
        send_ack(frame, StatusCode::BadState);
        return;
    }

    // el calendarizador arranca de cero con las tareas desplegadas
    Scheduler *chosen = &scheduler_edf;
    if (algorithm == Algorithm::RMS) {
        chosen = &scheduler_rms;
    }
    if (!chosen->configure(tasks, task_count, ut_per_tick)) {
        send_ack(frame, StatusCode::Error);
        return;
    }

    // el timer se reinicia aquí para que el tick 0 sea este instante
    if (!tick_timer_start(tick_us)) {
        send_ack(frame, StatusCode::Error);
        return;
    }

    scheduler = chosen;
    report_ticks = requested_report_ticks;
    next_report = requested_report_ticks;
    deadline_misses = 0;
    usb_reset_counters();
    running_task = NO_TASK;
    tick_count = 0;
    state = NodeState::Running;
    send_ack(frame, StatusCode::Ok);
}

// STOP: detiene el timer
static void handle_stop(const Frame *frame){
    // STOP no lleva carga
    if (frame->length != 0) {
        send_ack(frame, StatusCode::Error);
        return;
    }

    // solo se detiene si está corriendo
    if (state != NodeState::Running) {
        send_ack(frame, StatusCode::BadState);
        return;
    }

    tick_timer_stop();
    state = NodeState::Stopped;

    led = false;
    digitalWrite(LED_BUILTIN, led);

    send_ack(frame, StatusCode::Ok);
}

void node_setup(){
    // LED integrado (GPIO 25 en la Pico H)
    pinMode(LED_BUILTIN, OUTPUT);
}

void node_handle_frame(const Frame *frame){
    if (frame->type == (uint8_t)MessageType::CalibrateReq) {
        handle_calibrate_req(frame);
        return;
    }
    if (frame->type == (uint8_t)MessageType::DeployTasks) {
        handle_deploy_tasks(frame);
        return;
    }
    if (frame->type == (uint8_t)MessageType::Start) {
        handle_start(frame);
        return;
    }
    if (frame->type == (uint8_t)MessageType::Stop) {
        handle_stop(frame);
        return;
    }

    // un ACK del host no se confirma (si no, se responderían para siempre)
    if (frame->type == (uint8_t)MessageType::Ack) {
        return;
    }

    // SPORADIC_RELEASE (aún sin hacer) y tipos desconocidos:
    // se responde "no implementado"
    send_ack(frame, StatusCode::NotImplemented);
}

void node_tick(){
    // el timer solo corre después de START, que elige el calendarizador
    if (scheduler == nullptr) {
        return;
    }

    // el calendarizador numera los ticks desde 0: el primero es el de START
    const uint32_t now = tick_count;
    const TickDecision decision = scheduler->tick(now);

    // ejecutar el tramo de trabajo de este tick
    for (uint32_t i = 0; i < decision.units; i++) {
        unidad_trabajo();
    }
    running_task = decision.task_id;
    deadline_misses += decision.missed_count;

    tick_count++;

    // cada LED_EVERY_TICKS ticks se cambia el LED
    if (tick_count % LED_EVERY_TICKS == 0) {
        led = !led;
        digitalWrite(LED_BUILTIN, led);
    }

    // cada report_ticks ticks se avisa al host (con 0 no se avisa).
    // El reporte lleva el tick recién procesado.
    if (tick_report_due(report_ticks, now, &next_report)) {
        static Frame report;
        TickReportData data;

        data.tick = now;
        data.deadline_misses = deadline_misses;
        data.sporadic_ignored = 0; // todavía no hay tarea esporádica
        data.ticks_lost = 0; // todavía no se detectan
        data.events_dropped = usb_events_dropped();
        data.bad_frames = usb_bad_frames();
        data.max_busy_us = 0; // todavía no se mide
        data.max_wake_latency_us = 0; // todavía no se mide

        build_tick_report(&report, &data);
        usb_send(&report);
    }
}

uint32_t node_tick_count(){
    return tick_count;
}
