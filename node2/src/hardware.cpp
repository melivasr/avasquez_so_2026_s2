// LEDS Y BOTÓN
#include <Arduino.h>

#include "hardware.h"

// LEDS: bit 0 en GP10 ... bit 3 en GP13
static const uint8_t LED_SLOT_PINS[4] = {10, 11, 12, 13};
static const uint8_t LED_MISS_PIN = 14;
static const uint8_t BUTTON_PIN = 15;

// Antirrebote: una pulsación vale si el flanco de bajada llega al menos
// este tiempo después del flanco anterior (de cualquier tipo)
#define BUTTON_DEBOUNCE_US 50000U

// Los escribe solo la interrupción
static volatile uint32_t button_presses = 0;
static volatile uint32_t last_edge_us = 0;

// Pulsaciones ya entregadas (solo el ciclo principal)
static uint32_t seen_presses = 0;

// Interrupción del botón: solo registra la pulsación.
// El calendarizador hace el resto.
static void on_button_edge(){
    const uint32_t now = time_us_32();
    const uint32_t gap = now - last_edge_us;
    last_edge_us = now;

    // presionado (pin en 0) y lejos del flanco anterior
    if (digitalRead(BUTTON_PIN) == LOW &&
        gap >= BUTTON_DEBOUNCE_US) {
        button_presses = button_presses + 1;
    }
}

void hardware_setup(){
    for (uint8_t i = 0; i < 4; i++) {
        pinMode(LED_SLOT_PINS[i], OUTPUT);
    }
    pinMode(LED_MISS_PIN, OUTPUT);

    hardware_show_slot(0);
    hardware_set_miss(false);

    // suelto = 1 (pull-up), presionado = 0
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), on_button_edge, CHANGE);
}

void hardware_show_slot(uint32_t slot){
    for (uint32_t bit = 0; bit < 4; bit++) {
        digitalWrite(LED_SLOT_PINS[bit], (slot >> bit) & 1U);
    }
}

void hardware_set_miss(bool on){
    digitalWrite(LED_MISS_PIN, on);
}

uint32_t hardware_take_presses(){
    const uint32_t presses = button_presses;
    const uint32_t fresh = presses - seen_presses;
    seen_presses = presses;
    return fresh;
}
