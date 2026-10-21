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

// Eventos pendientes de enviar y cuántos se descartaron por cola llena
static EventQueue events;
static uint32_t dropped_events = 0;

// TICK_REPORT que espera a que salgan los eventos anteriores.
// Si llega otro antes de enviarlo, el nuevo reemplaza al viejo.
static Frame pending_report;
static bool report_pending = false;

// Trama que sale al final, después de los eventos y el reporte pendientes
// (el ACK de STOP, para que el host reciba todo lo anterior primero)
static Frame pending_last;
static bool last_pending = false;

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

void usb_add_event(EventType type, uint32_t tick, uint16_t task_id, uint16_t value){
    NodeEvent event;

    event.tick = tick;
    event.type = type;
    event.task_id = task_id;
    event.value = value;

    // cola llena: no se espera, se descarta el nuevo y se cuenta
    if (!events.push(event)) {
        dropped_events++;
    }
}

void usb_post_report(const Frame *report){
    pending_report = *report;
    report_pending = true;
}

void usb_post_last(const Frame *frame){
    pending_last = *frame;
    last_pending = true;
}

void usb_send_events(){
    static Frame message;

    while (!events.empty()) {
        // se arma el lote con los eventos que caben en el espacio libre del USB
        const int space = Serial.availableForWrite() - (int)FRAME_MIN_SIZE - 1;
        if (space < (int)EVENT_RECORD_SIZE) {
            // sin espacio: los eventos quedan en la cola para después
            return;
        }

        build_event_batch(&message, &events, (size_t)space / EVENT_RECORD_SIZE);
        usb_send(&message);
    }

    // el reporte sale después de los eventos 
    if (report_pending) {
        if (Serial.availableForWrite() < (int)(FRAME_MIN_SIZE + pending_report.length)) {
            return;
        }
        usb_send(&pending_report);
        report_pending = false;
    }

    // y al final la trama que tenía que esperar a todo lo anterior
    if (last_pending) {
        if (Serial.availableForWrite() < (int)(FRAME_MIN_SIZE + pending_last.length)) {
            return;
        }
        usb_send(&pending_last);
        last_pending = false;
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

void usb_clear_events(){
    NodeEvent event;

    while (events.pop(event)) {
    }
    report_pending = false;
}
