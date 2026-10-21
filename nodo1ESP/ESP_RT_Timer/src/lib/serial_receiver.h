#ifndef SERIAL_RECEIVER_H
#define SERIAL_RECEIVER_H

#include <stdint.h>

#include "protocol.h"

void serial_receiver_run(void);

// Codifica y envía una trama completa por la UART. Seguro entre tareas:
// una trama nunca se mezcla con otra. false si falla.
bool serial_send_frame(MessageType type, const uint8_t *payload, uint16_t length);

#endif