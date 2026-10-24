# Nodo 2 — Raspberry Pi Pico H

Firmware C++ con núcleo arduino-pico sobre Pico SDK, sin calendarización RTOS.
El timer de hardware marca ticks; el calendarizador local ejecuta RMS o EDF.
Incluye calibración, ocho tareas, una esporádica, botón, LEDs y cola de eventos.
Conserva trabajos solapados FIFO y calcula plazos con tiempo interno de 64 bits.
La capacidad de 64 trabajos se valida antes de configurar o insertar tareas:
`sum(ceil(D/T) + 1) <= 64`, con margen para ticks perdidos.

Comparte el [protocolo serial](../host/docs/PROTOCOL.md) con host y ESP32.
ADD_TASK (0x0A) agrega tareas entre ticks sin reiniciar el reloj ni los trabajos.
DEPLOY con carga vacía borra la tabla estando detenido. Criticidad es int32.

Compilar/cargar con PlatformIO desde este checkout completo:

```sh
pio run -d node2 -e pico
pio run -d node2 -e pico -t upload
```

Cerrar el monitor serial antes de arrancar el host. El archivo platformio.ini
selecciona el núcleo Earle Philhower y una Raspberry Pi Pico RP2040.

LEDs de ranura: GP10..GP13; plazo perdido: GP14; botón a GND en GP15 con pull-up.
El [flujo de prueba física](../host/docs/HARDWARE_TEST.md) explica la integración.
`make -C host test` verifica módulos puros y ejecuta este firmware con interfaces
USB/reloj/GPIO sustituidas en Linux; no valida el BSP ni hardware físico.
