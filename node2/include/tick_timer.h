// Tick del calendarizador con un timer de hardware.
// La interrupción solo cuenta ticks y anota cuándo pasó el último;
// el ciclo principal decide qué hacer con ellos.
#pragma once

#include <stdint.h>

#define TICK_DEFAULT_US 10000U

// Arranca (o reinicia) el timer con un periodo de tick_us microsegundos.
// El conteo vuelve a 0: el instante de la llamada es el tick 0.
// Devuelve false si tick_us es 0 o el timer no se pudo crear.
bool tick_timer_start(uint32_t tick_us);

// Detiene el timer
void tick_timer_stop();

// Ticks que marcó el timer desde tick_timer_start (el último tick alcanzado)
uint32_t tick_timer_count();

// Momento del último tick marcado (time_us_32, en microsegundos)
uint32_t tick_timer_last_us();
