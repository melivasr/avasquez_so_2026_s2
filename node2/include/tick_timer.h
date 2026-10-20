// Tick del calendarizador con un timer de hardware.
// La interrupción solo cuenta ticks; el ciclo principal los consume
#pragma once

#include <stdint.h>

#define TICK_DEFAULT_US 10000U

// Arranca (o reinicia) el timer con un periodo de tick_us microsegundos.
// Devuelve false si tick_us es 0 o el timer no se pudo crear.
bool tick_timer_start(uint32_t tick_us);

// Detiene el timer y descarta los ticks pendientes
void tick_timer_stop();

// Si hay un tick pendiente lo consume y devuelve true
bool tick_timer_take();

// Ticks que ocurrieron y todavía no se consumieron
uint32_t tick_timer_pending();
