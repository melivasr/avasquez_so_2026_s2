// Comunicación con el host por USB.
#pragma once

#include <stdint.h>

#include "event_queue.h"
#include "protocol.h"

// Envía una trama 
void usb_send(const Frame *frame);

// Lee los bytes que ya llegaron, sin esperar. Devuelve true cuando se
// completó una trama válida. Las tramas malas se descartan y se cuentan
// (ver usb_bad_frames).
bool usb_receive(Frame *frame);

// Guarda un evento con el tick actual para enviarlo después
void usb_add_event(EventType type, uint32_t task_id, uint32_t value);

// Envía los eventos guardados mientras el USB tenga espacio
void usb_send_events();

// Tramas descartadas (CRC, largo mayor a 512 o incompletas) desde el
// último usb_reset_counters()
uint32_t usb_bad_frames();

// Eventos descartados por cola llena desde el último usb_reset_counters()
uint32_t usb_events_dropped();

// Pone en 0 los contadores (se llama en START)
void usb_reset_counters();
