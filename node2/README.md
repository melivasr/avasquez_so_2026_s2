# Nodo 2 — Raspberry Pi Pico H

Firmware C++ con núcleo arduino-pico sobre Pico SDK.

Comparte el [protocolo serial](../host/docs/PROTOCOL.md) con host y ESP32.
ADD_TASK (0x0A) agrega tareas entre ticks sin reiniciar el reloj ni los trabajos.
DEPLOY con carga vacía borra la tabla estando detenido. Criticidad es int32.

El nodo recibe del host la lista de tareas, mide cuánto trabajo cabe en un
tick, y a partir del `START` calendariza esas tareas con RMS o EDF. Cada cambio
de tarea, liberación, finalización o plazo perdido se le reporta al host como
evento, y además se refleja en los LEDs de la protoboard.

## Conexiones

| Pin  | Función | Notas |
|------|---------|-------|
| GP10 | LED bit 0 | ID de la tarea en ejecución, en binario |
| GP11 | LED bit 1 | |
| GP12 | LED bit 2 | |
| GP13 | LED bit 3 | |
| GP14 | LED de plazo perdido | se enciende 200 ms con cada `MISS` |
| GP15 | Botón | a GND; usa el pull-up interno |

Cada LED va con su resistencia en serie hacia GND. Los cuatro primeros muestran
el ID de la tarea que está corriendo. Con todos apagados, el nodo está
ocioso. El botón dispara la tarea esporádica.

## El USB (TinyUSB)

El RP2040
tiene el controlador USB adentro, y el que lo maneja es TinyUSB, la pila USB
que trae el Pico SDK. arduino-pico la configura como un dispositivo CDC
(puerto serie virtual), por eso en Linux aparece como `/dev/ttyACM0` y el host
la abre como cualquier puerto serial.

El núcleo deja una alarma de hardware
que cada 1 ms dispara una interrupción y ahí corre `tud_task()`, que es lo que
mueve los bytes entre los búferes de TinyUSB y el bus. Desde el código
solo se usa la interfaz `Serial` de Arduino:

- `Serial.available()` / `Serial.read()` para leer lo que ya llegó, sin esperar.
- `Serial.availableForWrite()` para saber cuánto espacio libre hay en el búfer
  de salida de TinyUSB (`tud_cdc_write_available()` por debajo).
- `Serial.write()` para mandar una trama completa.

`Serial.write()` se bloquea si el búfer está
lleno, y eso rompería la regla de no quedarse esperando. Por eso
`usb_communication.cpp` siempre pregunta primero con `availableForWrite()` y
solo escribe si la trama cabe entera. Si no cabe, la guarda y la intenta en la
siguiente vuelta del ciclo principal. Lo mismo al leer, se procesan solo los
bytes que ya están, y una trama que se queda a medias más de 100 ms se descarta
y se cuenta como mala.

Como el USB puede entregar media trama, una trama o varias pegadas, los bytes
pasan uno por uno por `FrameReceiver`, que busca el inicio `A5 5A`, junta el
largo y la carga útil, y revisa el CRC-16 antes de aceptarla.

## Cómo funciona

`loop()`:

1. `node_tick()`: si el timer marcó un tick nuevo, lo procesa.
2. `usb_receive()`: si llegó una trama completa del host, la atiende.
3. `usb_send_events()`: envía lo que esté pendiente, según el espacio libre.

El tick viene de `add_repeating_timer_us` del Pico SDK. La interrupción solo
suma 1 al contador y anota la hora, el trabajo lo hace el ciclo
principal. Si el ciclo se atrasa, salta al tick actual y reporta los que se
saltó como `TICK_LOST`.

Se vencen los trabajos que pasaron su plazo (`MISS`), se
liberan los trabajos nuevos, se atienden las pulsaciones del botón, el
calendarizador elige qué trabajo corre, y ese trabajo ejecuta su cuota de
unidades de trabajo (UT). Cuando completa sus `W` UT termina (`COMPLETE`).

### Archivos

| Archivo | Qué hace |
|---------|----------|
| `src/main.cpp` | Punto de entrada: `setup()` y el ciclo principal. |
| `src/node.cpp` | Estado del nodo. Responde a cada mensaje del host (calibrar, desplegar, agregar tarea, iniciar, detener) y procesa cada tick. |
| `src/scheduler/scheduler.cpp` | tabla de tareas, trabajos activos, liberaciones, plazos, esporádica. |
| `src/scheduler/scheduler_rms.cpp` | Prioridad fija: menor período primero. |
| `src/scheduler/scheduler_edf.cpp` | Prioridad dinámica: plazo absoluto más cercano primero. |
| `src/tick_timer.cpp` | Timer de hardware que marca los ticks. |
| `src/calibration.cpp` | Mide cuántas UT caben en un tick y devuelve el 80 % del promedio. |
| `src/ut.c` | La unidad de trabajo: un cálculo fijo que ocupa CPU. |
| `src/usb_communication.cpp` | Lectura y escritura por USB sin bloquear, colas de respuestas y eventos. |
| `src/frame_receiver.cpp` | Arma tramas byte a byte y valida el CRC. |
| `src/protocol.cpp`, `src/crc16.cpp` | Codificación de tramas y CRC-16. |
| `src/messages.cpp` | Arma las cargas útiles ACK, `TICK_REPORT`, lotes de eventos. |
| `src/event_queue.cpp` | Cola circular de 256 eventos pendientes de envío. |
| `src/task_parser.cpp` | Convierte las líneas `id,tipo,W,T,D,criticidad` en tareas. |
| `src/hardware.cpp` | LEDs y botón (con su interrupción y antirrebote). |


### Algunos límites

- Hasta 8 tareas, de las cuales a lo sumo una es esporádica. Los IDs van de 1
  a 65535 (el 0 significa "ninguna tarea").
- Los trabajos activos se guardan en un arreglo fijo de 64 (no hay `malloc`
  mientras corre).
- Si un trabajo nuevo llega mientras el anterior de la misma tarea sigue
  activo, se conservan los dos y se atienden en orden de llegada.
- Los tiempos internos son de 64 bits.
- `ADD_TASK` agrega una tarea entre ticks sin reiniciar el reloj ni los
  trabajos que ya estaban. Un `DEPLOY` vacío borra la tabla (solo detenido).

## Compilar y cargar

Compilar/cargar con PlatformIO desde este checkout completo:

```sh
pio run -d node2 -e pico               # compilar
pio run -d node2 -e pico -t upload     # compilar y cargar
```

`platformio.ini` ya elige el núcleo de Earle Philhower y la placa `rpipico`, y
carga por `/dev/ttyACM0`. 

## Probar en la placa

`tools/ver_leds.py` hace de host mínimo de prueba: calibra, despliega un CSV, da `START`
y va mostrando en la terminal cómo deberían verse los LEDs, para compararlo
con la placa.

```sh
pip install pyserial
python3 node2/tools/ver_leds.py node2/tools/tareas_demo.csv --algoritmo EDF --segundos 10
```
