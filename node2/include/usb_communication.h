// Comunicación con el host por USB.
#pragma once

#include <stdint.h>

#include "event_queue.h"
#include "protocol.h"

// Envía una respuesta de control o la conserva en una cola acotada.
void usb_send(const Frame *frame);

// Lee los bytes que ya llegaron, sin esperar. Devuelve true cuando se
// completó una trama válida. Las tramas malas se descartan y se cuentan
// (ver usb_bad_frames).
bool usb_receive(Frame *frame);

// Guarda un evento para enviarlo después. Si la cola está llena se
// descarta y se cuenta (ver usb_events_dropped).
void usb_add_event(EventType type, uint32_t tick, uint16_t task_id, uint16_t value);

// Guarda un TICK_REPORT; durante la ejecución tiene prioridad sobre eventos.
// Si ya había uno sin enviar, lo reemplaza.
void usb_post_report(const Frame *report);

// Guarda una trama para enviarla cuando ya salieron los eventos y el
// TICK_REPORT pendientes (el ACK de STOP)
void usb_post_last(const Frame *frame);

// Atiende respuestas de control, heartbeat y un lote de eventos sin esperar.
// En STOP vacía eventos y reporte antes de enviar usb_post_last.
void usb_send_events();

// Tramas descartadas (CRC, largo mayor a 512 o incompletas) desde el
// último usb_reset_counters()
uint32_t usb_bad_frames();

// Eventos descartados por cola llena desde el último usb_reset_counters()
uint32_t usb_events_dropped();

// Pone en 0 los contadores (se llama en START)
void usb_reset_counters();

// Descarta los eventos y el reporte pendientes (se llama en START)
void usb_clear_events();
