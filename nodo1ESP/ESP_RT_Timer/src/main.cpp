#include "lib/node_runtime.h"
#include "lib/serial_receiver.h"

extern "C" void app_main(void)
{
    // Crea el hilo calendarizador, el de eventos y configura LEDs y botón.
    // Si algo falla, quedan los 5 LEDs encendidos.
    node_runtime_init();

    // app_main se queda aquí para siempre atendiendo la UART.
    serial_receiver_run();
}