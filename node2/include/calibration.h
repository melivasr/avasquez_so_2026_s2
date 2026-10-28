// Calibración: cuántas unidades de trabajo (UT) caben en un tick.
#pragma once

#include <stdint.h>

// Ejecuta unidad_trabajo() durante 50 ticks de tick_us microsegundos y
// devuelve el 80 % del promedio de UT por tick (0 si no cabe ninguna).
// Bloquea mientras mide: con tick de 10 ms tarda ~0,5 s.
uint32_t calibration_measure(uint32_t tick_us);
