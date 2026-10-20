// Estado: Qué responde a cada mensaje del host y qué hace en cada tick.
#pragma once

#include <stdint.h>

#include "protocol.h"

// Configura el LED
void node_setup();

// Atiende una trama válida que llegó del host
void node_handle_frame(const Frame *frame);

// Lo que se hace en cada tick del timer
void node_tick();

// Ticks desde START (marca de tiempo de los eventos)
uint32_t node_tick_count();
