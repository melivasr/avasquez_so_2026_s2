// NODO 2 - RASPBERRY PI PICO H
// Atiende el tick del timer,
// las tramas que llegaron por USB y envía los eventos pendientes.

#include <Arduino.h>

#include "node.h"
#include "usb_communication.h"

static Frame received;

// Abre el USB (TinyUSB) y prepara LEDs y botón
void setup(){
    Serial.begin(115200);
    node_setup();
}

// Ciclo principal: ninguna de las tres partes se queda esperando
void loop(){
    // Procesar el tick nuevo que marcó el timer (si se atrasó, se salta
    // al tick actual y los que faltaron se cuentan como perdidos)
    node_tick();

    // Atender las tramas que ya llegaron
    if (usb_receive(&received)) {
        node_handle_frame(&received);
    }

    // Enviar los eventos pendientes 
    usb_send_events();
}
