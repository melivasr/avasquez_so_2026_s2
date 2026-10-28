#ifndef SERIAL_RECEIVER_H
#define SERIAL_RECEIVER_H

#include <stdint.h>

#include "protocol.h"

void serial_receiver_run(void);

// Codifica y envía una trama completa por la UART. Seguro entre tareas:
// una trama nunca se mezcla con otra. false si falla.
bool serial_send_frame(MessageType type, const uint8_t *payload, uint16_t length);

// Tramas descartadas por estar corruptas (CRC o longitud inválidos), pasarse
// del tamaño máximo o quedar incompletas (timeout entre bytes). El ruido antes
// de la marca de inicio no cuenta. Se reinicia con Start.
uint32_t serial_bad_frames(void);
void serial_bad_frames_reset(void);

#endif