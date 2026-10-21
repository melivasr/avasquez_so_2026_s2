#pragma once

#include <stdint.h>

// Configura los pines y la interrupción del botón; deja todo apagado
void hardware_setup();

// Muestra el binario en los 4 LEDs (0 = todos apagados)
void hardware_show_slot(uint32_t slot);

// Enciende o apaga el LED de plazo perdido
void hardware_set_miss(bool on);

// Pulsaciones del botón desde la última llamada
uint32_t hardware_take_presses();
