// Estado: Qué responde a cada mensaje del host y qué hace en cada tick.
#pragma once

#include <stdint.h>

#include "protocol.h"

// Configura los LEDs y el botón
void node_setup();

// Atiende una trama válida que llegó del host
void node_handle_frame(const Frame *frame);

// Si el timer marcó un tick nuevo, lo procesa. Se llama en cada vuelta
// del ciclo principal; sin tick nuevo no hace nada.
void node_tick();

// Próximo tick a procesar, contado desde START
uint32_t node_tick_count();
