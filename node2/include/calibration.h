// Calibración: cuántas unidades de trabajo (UT) caben en un tick.
#pragma once

#include <stdint.h>

// Ejecuta unidad_trabajo() durante tick_us microsegundos y cuenta
// cuántas veces terminó a tiempo
uint32_t calibration_measure(uint32_t tick_us);
