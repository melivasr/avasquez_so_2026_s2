// NODO 2 - RASPBERRY PI PICO H
// Atiende los ticks del timer,
// las tramas que llegaron por USB y envía los eventos pendientes.

#include <Arduino.h>

#include "node.h"
#include "tick_timer.h"
#include "usb_communication.h"

static Frame received;

void setup(){
    Serial.begin(115200);
    node_setup();
}

void loop(){
    // Procesar los ticks que marcó el timer, de a uno
    while (tick_timer_take()) {
        node_tick();
    }

    // Atender las tramas que ya llegaron
    while (usb_receive(&received)) {
        node_handle_frame(&received);
    }

    // Enviar los eventos pendientes 
    usb_send_events();
}
