
// CALIBRACIÓN
// Mide con micros() cuántas UT caben en un tick.
#include <Arduino.h>

#include "calibration.h"
extern "C" {
#include "ut.h"
}

uint32_t calibration_measure(uint32_t tick_us){
    uint32_t ut_per_tick = 0;
    const uint32_t start_us = micros();
    uint32_t elapsed_us = 0;

    // Ejecutar UT hasta que se acabe el tick
    while (elapsed_us < tick_us) {
        unidad_trabajo();
        elapsed_us = micros() - start_us;

        // solo cuenta si terminó dentro del tick
        if (elapsed_us <= tick_us) {
            ut_per_tick++;
        }
    }

    return ut_per_tick;
}
