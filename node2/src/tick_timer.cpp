// Usa add_repeating_timer_us del Pico SDK (alarma de hardware).
// La interrupción solo suma 1 a ticks_elapsed y anota la hora; el ciclo
// principal compara ese conteo con el tick que le toca procesar.

#include <pico/time.h>
#include "tick_timer.h"

static repeating_timer_t timer;
static bool running = false;

// Los escribe solo la interrupción
static volatile uint32_t ticks_elapsed = 0;
static volatile uint32_t last_tick_us = 0;

// Interrupción del timer: solo registra el tick
static bool on_tick(repeating_timer_t *t){
    (void)t;
    last_tick_us = time_us_32();
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

    // con el timer detenido la interrupción no corre: se reinicia el conteo
    ticks_elapsed = 0;
    last_tick_us = time_us_32();

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
}

uint32_t tick_timer_count(){
    return ticks_elapsed;
}

uint32_t tick_timer_last_us(){
    return last_tick_us;
}
