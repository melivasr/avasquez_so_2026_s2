// CALIBRACIÓN
// Mide con micros() cuántas UT caben en un tick: cuenta las UT de una
// ventana de varios ticks, saca el promedio por tick y deja un margen
// para el calendarizador y el envío de eventos.
#include <Arduino.h>

#include "calibration.h"
extern "C" {
#include "ut.h"
}

// Ticks que dura la medición y porcentaje del tick que se reporta
#define CAL_WINDOW_TICKS 50U
#define CAL_MARGIN_PERCENT 80U

// Corre UT durante CAL_WINDOW_TICKS ticks y devuelve las UT por tick con margen
uint32_t calibration_measure(uint32_t tick_us){
    // con tick de 1 s la ventana es de 50 s: todavía cabe en 32 bits
    const uint32_t window_us = tick_us * CAL_WINDOW_TICKS;
    const uint32_t start_us = micros();
    uint64_t ut_count = 0;

    // Ejecutar UT hasta que se acabe la ventana
    while (micros() - start_us < window_us) {
        unidad_trabajo();
        ut_count++;
    }

    // promedio por tick, con el margen ya aplicado
    return (uint32_t)((ut_count * CAL_MARGIN_PERCENT) / (100ULL * CAL_WINDOW_TICKS));
}
