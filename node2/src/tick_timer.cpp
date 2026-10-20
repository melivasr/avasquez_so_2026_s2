// Usa add_repeating_timer_us del Pico SDK (alarma de hardware).
// La interrupción solo suma 1 a ticks_elapsed. El ciclo principal
// lleva ticks_taken; la diferencia son los ticks pendientes.

#include <pico/time.h>
#include "tick_timer.h"

static repeating_timer_t timer;
static bool running = false;

static volatile uint32_t ticks_elapsed = 0; // lo escribe la interrupción
static uint32_t ticks_taken = 0; // lo escribe el ciclo principal

// Interrupción del timer: solo registra el tick
static bool on_tick(repeating_timer_t *t){
    (void)t;
    ticks_elapsed = ticks_elapsed + 1;

    // true = seguir repitiendo
    return true;
}

bool tick_timer_start(uint32_t tick_us){
    // un periodo de 0 no tiene sentido
    if (tick_us == 0) {
        return false;
    }

    // si ya corría se detiene para cambiar el periodo
    tick_timer_stop();

    // negativo: el periodo se mide entre inicios de cada llamada,
    // así el tick no se atrasa aunque on_tick tarde un poco
    const int64_t period_us = -(int64_t)tick_us;
    if (!add_repeating_timer_us(period_us, on_tick, nullptr, &timer)) {
        return false;
    }

    running = true;
    return true;
}

void tick_timer_stop(){
    // solo se cancela si había un timer activo
    if (running) {
        cancel_repeating_timer(&timer);
        running = false;
    }

    // se descartan los ticks pendientes
    ticks_taken = ticks_elapsed;
}

bool tick_timer_take(){
    // no hay ticks sin consumir
    if (tick_timer_pending() == 0) {
        return false;
    }

    ticks_taken = ticks_taken + 1;
    return true;
}

uint32_t tick_timer_pending(){
    // la diferencia entre lo que marcó la interrupción y lo que consumió el ciclo principal
    return ticks_elapsed - ticks_taken;
}
