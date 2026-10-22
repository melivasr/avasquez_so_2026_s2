#include "lib/node_runtime.h"
#include "lib/serial_receiver.h"

extern "C" void app_main(void)
{
    node_runtime_init(); // si falla, quedan los 5 LEDs encendidos
    serial_receiver_run();
}