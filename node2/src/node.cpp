#include <Arduino.h>

#include "node.h"
#include "calibration.h"
#include "hardware.h"
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

// Medidas de tiempo desde START (van en TICK_REPORT)
static uint32_t ticks_lost = 0; // ticks que no se alcanzaron a procesar
static uint32_t max_busy_us = 0; // lo más que tardó en procesarse un tick
static uint32_t max_wake_latency_us = 0; // lo más que tardó en atenderse un tick

// Tareas desplegadas por el host
static Task tasks[MAX_TASKS];
static size_t task_count = 0;

// Calendarizadores. En START se elige uno según el algoritmo que trae
static SchedulerRms scheduler_rms;
static SchedulerEdf scheduler_edf;
static Scheduler *scheduler = nullptr;

static bool led = false;

// El LED de plazo perdido se enciende con cada MISS y se apaga solo
// después de este tiempo (cada MISS nuevo lo vuelve a contar)
#define MISS_PULSE_MS 200U
static bool miss_lit = false;
static uint32_t miss_on_ms = 0;

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
    if (frame->length != 0 &&
        !parse_task_list((const char *)frame->payload, frame->length, pending, MAX_TASKS, &count)) {
        send_ack(frame, StatusCode::Error);
        return;
    }

    if (!rt_jobs_fit(pending, count)) {
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

// La tarea según su posición;
// 0 si no hay tarea. Es lo que muestran los LEDs.
static uint32_t slot_for_id(uint32_t task_id){
    for (size_t i = 0; i < task_count; i++) {
        if (tasks[i].id == task_id) {
            return (uint32_t)(i + 1);
        }
    }
    return 0;
}

// Procesa el tick indicado: calendarizador, trabajo y eventos
static void process_tick(uint32_t now){
    // las pulsaciones nuevas del botón se atienden en este tick
    const uint32_t presses = hardware_take_presses();
    if (presses > 0) {
        scheduler->sporadic_press(presses);
    }

    const TickDecision decision = scheduler->tick(now);

    // ejecutar el tramo de trabajo de este tick
    for (uint32_t i = 0; i < decision.units; i++) {
        unidad_trabajo();
    }
    deadline_misses += decision.missed_count;

    // los eventos del tick quedan en la cola; salen en segundo plano.
    // Los LEDs siguen a los eventos: SWITCH cambia y MISS
    // enciende el LED de plazo perdido por MISS_PULSE_MS
    for (size_t i = 0; i < decision.event_count; i++) {
        const NodeEvent &event = decision.events[i];
        usb_add_event(event.type, event.tick, event.task_id, event.value);

        if (event.type == EventType::Switch) {
            hardware_show_slot(slot_for_id(event.task_id));
        } else if (event.type == EventType::Miss) {
            hardware_set_miss(true);
            miss_lit = true;
            miss_on_ms = millis();
        }
    }

    // ya pasó el tiempo desde el último MISS: se apaga
    if (miss_lit &&
        millis() - miss_on_ms >= MISS_PULSE_MS) {
        hardware_set_miss(false);
        miss_lit = false;
    }

    // cada LED_EVERY_TICKS ticks se cambia el LED
    if ((now + 1) % LED_EVERY_TICKS == 0) {
        led = !led;
        digitalWrite(LED_BUILTIN, led);
    }
}

// Si toca, deja listo el TICK_REPORT del tick recién procesado
// (cada report_ticks ticks; con 0 no se avisa)
static void report_if_due(uint32_t now){
    if (tick_report_due(report_ticks, now, &next_report)) {
        static Frame report;
        TickReportData data;

        data.tick = now;
        data.deadline_misses = deadline_misses;
        data.sporadic_ignored = scheduler->sporadic_ignored();
        data.ticks_lost = ticks_lost;
        data.events_dropped = usb_events_dropped();
        data.bad_frames = usb_bad_frames();
        data.max_busy_us = max_busy_us;
        data.max_wake_latency_us = max_wake_latency_us;

        build_tick_report(&report, &data);
        usb_post_report(&report);
    }
}

// ADD_TASK: agrega entre ticks sin reemplazar las tareas anteriores.
static void handle_add_task(const Frame *frame){
    size_t length = frame->length;
    if (length > 0 && frame->payload[length - 1] == '\n') --length;
    Task task = {};
    if (task_count >= MAX_TASKS ||
        !parse_task_line((const char *)frame->payload, length, &task)) {
        send_ack(frame, StatusCode::Error);
        return;
    }
    for (size_t i = 0; i < task_count; ++i) {
        if (tasks[i].id == task.id ||
            (task.type == TaskType::Sporadic && tasks[i].type == TaskType::Sporadic)) {
            send_ack(frame, StatusCode::Error);
            return;
        }
    }
    if (!rt_jobs_fit(tasks, task_count, &task)) {
        send_ack(frame, StatusCode::Error);
        return;
    }
    if (state == NodeState::Running) {
        if (!scheduler->add_task(task, tick_count)) {
            send_ack(frame, StatusCode::Error);
            return;
        }
        usb_add_event(EventType::TaskAdded, tick_count, (uint16_t)task.id,
                      (uint16_t)(task_count + 1));
    }
    tasks[task_count++] = task;
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
    ticks_lost = 0;
    max_busy_us = 0;
    max_wake_latency_us = 0;
    usb_reset_counters();
    usb_clear_events();

    hardware_show_slot(0);
    hardware_set_miss(false);
    miss_lit = false;
    hardware_take_presses();

    state = NodeState::Running;
    send_ack(frame, StatusCode::Ok);

    // el tick 0 se procesa ya; la primera alarma
    // del timer marca el tick 1
    process_tick(0);
    tick_count = 1;
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

    // ya no corre ninguna tarea y sin ticks el pulso de plazo perdido
    // no se apagaría solo: todo apagado
    hardware_show_slot(0);
    hardware_set_miss(false);
    miss_lit = false;

    // el ACK sale después de los eventos pendientes
    // (se envían en segundo plano, sin esperar aquí)
    static Frame reply;
    build_ack(&reply, frame->type, StatusCode::Ok);
    usb_post_last(&reply);
}

// SPORADIC_RELEASE: el host pide liberar la tarea esporádica (como el botón).
// El ACK Ok solo dice que la pulsación quedó registrada; en el próximo tick
// el calendarizador decide si libera o ignora, y lo avisa con un evento.
static void handle_sporadic_release(const Frame *frame){
    // SPORADIC_RELEASE no lleva carga
    if (frame->length != 0) {
        send_ack(frame, StatusCode::Error);
        return;
    }

    // solo con el calendarizador corriendo
    if (state != NodeState::Running) {
        send_ack(frame, StatusCode::BadState);
        return;
    }

    // el nodo no tiene tarea esporádica
    if (!scheduler->has_sporadic()) {
        send_ack(frame, StatusCode::Error);
        return;
    }

    scheduler->sporadic_press(1);
    send_ack(frame, StatusCode::Ok);
}

void node_setup(){
    pinMode(LED_BUILTIN, OUTPUT);

    // LEDs del binario y de plazo perdido, y botón de la tarea esporádica
    hardware_setup();
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
    if (frame->type == (uint8_t)MessageType::AddTask) {
        handle_add_task(frame);
        return;
    }
    if (frame->type == (uint8_t)MessageType::Stop) {
        handle_stop(frame);
        return;
    }
    if (frame->type == (uint8_t)MessageType::SporadicRelease) {
        handle_sporadic_release(frame);
        return;
    }

    // un ACK del host no se confirma (si no, se responderían para siempre)
    if (frame->type == (uint8_t)MessageType::Ack) {
        return;
    }

    // tipos desconocidos: se responde "no implementado"
    send_ack(frame, StatusCode::NotImplemented);
}


void node_tick(){
    // solo con el calendarizador corriendo
    if (state != NodeState::Running) {
        return;
    }

    // primero el timer y después la hora: si la alarma entra entre las dos
    // lecturas, last_us igual queda antes de start_us y la resta no da negativa
    const uint32_t reached = tick_timer_count();
    const uint32_t last_us = tick_timer_last_us();
    const uint32_t start_us = micros();

    // el timer todavía no marcó el tick que sigue
    if ((int32_t)(reached - tick_count) < 0) {
        return;
    }

    // cuánto tardó el ciclo principal en atender la alarma
    const uint32_t latency = start_us - last_us;
    if (latency > max_wake_latency_us) {
        max_wake_latency_us = latency;
    }

    // se atrasó: los ticks que faltaron se cuentan y se salta al actual
    // 
    if (reached != tick_count) {
        const uint32_t lost = reached - tick_count;
        ticks_lost += lost;
        usb_add_event(EventType::TickLost, reached, NO_TASK,
                      (uint16_t)(lost > 0xFFFFU ? 0xFFFFU : lost));
        tick_count = reached;
    }

    const uint32_t now = tick_count;
    tick_count++;
    process_tick(now);

    // cuánto tardó este tick; se mide antes del reporte para que lo incluya
    const uint32_t busy = micros() - start_us;
    if (busy > max_busy_us) {
        max_busy_us = busy;
    }

    report_if_due(now);
}

uint32_t node_tick_count(){
    return tick_count;
}
