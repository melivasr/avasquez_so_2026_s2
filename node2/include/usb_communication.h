// Comunicación con el host por USB.
#pragma once

#include <stdint.h>

#include "event_queue.h"
#include "protocol.h"

// Envía una trama 
void usb_send(const Frame *frame);

// Lee los bytes que ya llegaron, sin esperar. Devuelve true cuando se
// completó una trama válida. Las tramas malas se descartan y se reportan
// con un evento FRAME_ERROR.
bool usb_receive(Frame *frame);

// Guarda un evento con el tick actual para enviarlo después
void usb_add_event(EventType type, uint32_t task_id, uint32_t value);

// Envía los eventos guardados mientras el USB tenga espacio
void usb_send_events();
