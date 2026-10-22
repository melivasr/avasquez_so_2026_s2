#ifndef CALIBRATION_H
#define CALIBRATION_H

#include <stdint.h>

// Devuelve UT por tick, o 0 si falla.
uint32_t calibration_measure(uint32_t tick_us);

#endif