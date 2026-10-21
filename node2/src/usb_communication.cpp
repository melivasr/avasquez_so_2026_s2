// COMUNICACIÓN USB
// Recibe tramas byte a byte y envía tramas y eventos.

#include <Arduino.h>

#include "usb_communication.h"
#include "frame_receiver.h"
#include "messages.h"
#include "node.h"

// Tiempo máximo sin bytes con una trama a medias
#define FRAME_TIMEOUT_MS 100U

static FrameReceiver receiver;

// Momento del último byte recibido
static uint32_t last_byte_ms = 0;

// Tramas descartadas (CRC, largo o incompletas) desde el último reinicio.
// Van en TICK_REPORT.
static uint32_t bad_frames = 0;

// Errores del receptor ya sumados a bad_frames, para notar uno nuevo
static uint32_t counted_crc_errors = 0;
static uint32_t counted_length_errors = 0;

// Eventos pendientes de enviar y cuántos se perdieron por cola llena
// (lost_events se avisa con EVENTS_LOST; dropped_events es el total)
static EventQueue events;
static uint32_t lost_events = 0;
static uint32_t dropped_events = 0;

void usb_send(const Frame *frame){
    static uint8_t out[FRAME_MAX_SIZE];

    const size_t size = encode_frame(frame, out, sizeof(out));

    // si el USB no tiene espacio (host no lee) se descarta en vez de esperar
    if (Serial.availableForWrite() < (int)size) {
        return;
    }

    // write envía los bytes
    Serial.write(out, size);
}

// Si el receptor descartó tramas (CRC malo o largo mayor a 512), se cuentan
static void count_receiver_errors(){
    bad_frames += receiver.crc_errors() - counted_crc_errors;
    bad_frames += receiver.length_errors() - counted_length_errors;

    counted_crc_errors = receiver.crc_errors();
    counted_length_errors = receiver.length_errors();
}

bool usb_receive(Frame *frame){
    // Procesar solo los bytes que ya llegaron (no se queda esperando)
    while (Serial.available() > 0) {
        const uint8_t byte = Serial.read();
        last_byte_ms = millis();

        const bool valid = receiver.push(byte, frame);
        count_receiver_errors();

        // trama completa: los bytes que sigan se leen 
        if (valid) {
            return true;
        }
    }

    // Trama a medias y dejaron de llegar bytes: se descarta
    if (receiver.in_progress() &&
        millis() - last_byte_ms >= FRAME_TIMEOUT_MS) {
        receiver.reset();
        bad_frames++;
    }

    return false;
}

void usb_add_event(EventType type, uint32_t task_id, uint32_t value){
    NodeEvent event;

    event.tick = node_tick_count();
    event.type = type;
    event.task_id = task_id;
    event.value = value;

    // cola llena: no se espera, solo se cuenta el evento perdido
    if (!events.push(event)) {
        lost_events++;
        dropped_events++;
    }
}

void usb_send_events(){
    static Frame message;
    const int event_frame_size = FRAME_MIN_SIZE + EVENT_SIZE;

    // si se perdieron eventos y ya hay espacio, se avisa cuántos
    if (lost_events > 0 &&
        !events.full()) {
        NodeEvent lost;

        lost.tick = node_tick_count();
        lost.type = EventType::EventsLost;
        lost.task_id = NO_TASK;
        lost.value = lost_events;

        events.push(lost);
        lost_events = 0;
    }

    while (!events.empty()) {
        // el USB no tiene espacio: el evento queda en la cola para después
        if (Serial.availableForWrite() < event_frame_size) {
            return;
        }

        NodeEvent event;

        events.pop(event);
        build_event(&message, &event);
        usb_send(&message);
    }
}

uint32_t usb_bad_frames(){
    return bad_frames;
}

uint32_t usb_events_dropped(){
    return dropped_events;
}

void usb_reset_counters(){
    bad_frames = 0;
    dropped_events = 0;
}
