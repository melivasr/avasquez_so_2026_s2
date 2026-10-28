# Contrato serial del nodo ESP32

Este documento describe lo que el host necesita saber del nodo ESP32: el protocolo serial, los límites y el comportamiento observable. No hace falta conocer el interior del firmware para programar contra él.

El contrato común a los dos nodos (ESP32 y Raspberry Pi Pico) está en [host/docs/PROTOCOL.md](../../../host/docs/PROTOCOL.md) y sus constantes en [common/rt_protocol.h](../../../common/rt_protocol.h). Este archivo documenta cómo lo cumple el ESP32 y lo que se verificó en la placa.

## 1. Enlace físico

- UART0 vía USB-serial, 115200 baudios, 8N1, sin control de flujo.
- El puerto es exclusivo del protocolo: el nodo no imprime texto de depuración. Todo lo que sale es una trama, salvo el ruido de arranque (sección 6).
- En macOS el puerto de desarrollo fue `/dev/cu.usbserial-0001`; en Linux suele ser `/dev/ttyUSB0`.

## 2. Formato de trama

```
[A5 5A][tipo: 1 B][longitud: 2 B LE][carga útil][CRC-16: 2 B LE]
```

- **Marca de inicio:** `0xA5 0x5A`.
- **CRC:** CRC-16 CCITT-FALSE (polinomio 0x1021, valor inicial 0xFFFF, sin XOR final), calculado sobre tipo + longitud + carga útil. No incluye la marca.
- **Tamaño:** carga útil de hasta 512 bytes, trama de hasta 519.
- **Tramas inválidas:** si el CRC o la longitud no cuadran, el nodo la descarta sin responder y la cuenta en `tramas_malas` del TickReport.
- **Resincronización:** si una trama queda a medias, el nodo espera 100 ms entre bytes y luego la descarta. Después de un error de transmisión conviene esperar al menos 100 ms antes de reenviar.

Ejemplo, ACK de un despliegue aceptado: `a5 5a 09 02 00 03 00 4b 01`

## 3. Tipos de mensaje

| Tipo | Valor | Dirección |
|---|---|---|
| CalibrateReq | 0x01 | host → nodo |
| CalibrateResp | 0x02 | nodo → host |
| DeployTasks | 0x03 | host → nodo |
| Start | 0x04 | host → nodo |
| Event | 0x05 | nodo → host |
| TickReport | 0x06 | nodo → host |
| SporadicRelease | 0x07 | host → nodo |
| Stop | 0x08 | host → nodo |
| Ack | 0x09 | nodo → host |
| AddTask | 0x0A | host → nodo |

Un tipo que el nodo no atiende (los que solo envía él, o los no definidos) recibe `ACK(tipo, 2)`.

## 4. ACK (0x09)

Carga de 2 bytes: `[tipo original][estado]`.

| Estado | Significado |
|---|---|
| 0 | Aceptado |
| 1 | Rechazado (datos inválidos) |
| 2 | No implementado |
| 3 | Estado inválido para esa operación |

El estado 3 aparece en: `Start` con el nodo ya en ejecución o sin calibración previa; `Stop` sin ejecución; `DeployTasks` o `CalibrateReq` con el nodo en ejecución; `SporadicRelease` sin ejecución.

## 5. Calibración

**CalibrateReq (host → nodo).** Carga de 4 bytes: `tick_us` uint32 LE, entre 1000 y 1 000 000 (1 ms a 1 s).

**CalibrateResp (nodo → host).** Carga de 4 bytes: `ut_por_tick` uint32 LE (el `sn` del enunciado).

- El nodo ejecuta `unidad_trabajo()` sin parar durante 50 ticks de un temporizador de hardware y cuenta cuántas veces la ejecutó. Declara el 80 % de lo medido por tick, para dejar margen al calendarizador, a los eventos y a la UART.
- Con tick de 10 ms el resultado fue 70 UT/tick en 5 de 5 ejecuciones. Con tick de 200 ms, 1417 UT/tick. Ese 70 no se contrastó con una medición independiente.
- Dura unos 50 ticks (~0,5 s con tick de 10 ms, ~10 s con 200 ms). Durante ese tiempo el nodo no atiende otra cosa, así que el host debe esperar con un timeout de `50 × tick + margen` (en las pruebas se usó 5 s de margen) y no enviar otra orden hasta recibir la respuesta.
- Error: `ACK(0x01, 1)` si la longitud no es 4, si `tick_us` está fuera de rango o si la medición falla. Con el nodo en ejecución responde `ACK(0x01, 3)`.
- El valor no se guarda entre reinicios: hay que calibrar en cada ejecución. El `tick_us` de la última calibración exitosa es el que usa `Start`.

## 6. Apertura del puerto

- **Abrir el puerto desde Python reinicia la placa**, incluso con `dtr=False` y `rts=False`. Con un host en C sobre Linux no lo hemos comprobado; conviene suponer que también ocurre.
- Tras el reinicio sale texto de arranque de la ROM (304 bytes en 8 de 8 aperturas, más después de cargar el firmware). El host debe descartar bytes hasta ver unos 0,5 s de silencio y resincronizar por `A5 5A` + CRC. Leer una cantidad fija de bytes para vaciar el ruido falló.
- Cerrar el puerto sin `Stop` no reinicia el nodo ni detiene su ejecución. Una reconexión debe tratarse como un nodo nuevo: calibrar y desplegar otra vez.
- Dos veces, justo después de cargar el firmware, el primer ACK llegó con el encabezado dañado. No se reprodujo y no sabemos la causa. El host debe tratar "no llegó un ACK válido" como algo normal: timeout, reintento y resincronización.

## 7. DeployTasks (0x03)

Carga: texto ASCII, una tarea por línea, separadas solo con `\n` (con `\r\n` se rechazan).

```
id,tipo,W,T,D,criticidad
3,P,40,20,20,2
```

- `tipo` es `P` (periódica) o `S` (esporádica).
- `id`, `W`, `T`, `D` son enteros decimales sin signo y mayores que 0. `T` y `D` van en ticks y `W` en UT. `id` no puede pasar de 65535.
- `criticidad` es un entero de 32 bits **con signo** (puede llevar `-`). Sirve para desempatar (sección 12).
- Formato estricto: exactamente 6 campos con una sola coma entre ellos. Se rechazan campos vacíos, comas dobles, al inicio o al final, espacios, `\r`, tipo en minúscula, campos de más y números que no caben en 32 bits. Una línea mide como máximo 95 caracteres.
- **Todo o nada:** si una línea es inválida no se despliega nada, se responde `ACK(0x03, 1)` y la tabla anterior queda intacta.
- Máximo 8 tareas, `id` únicos y una sola esporádica por nodo.
- **Cola de trabajos:** el nodo tiene 64 registros de trabajo y reserva `ceil(D/T) + 1` por tarea. Si la suma pasa de 64 se rechaza el despliegue. Esto solo importa con `D` muy grande respecto a `T`.
- Un despliegue **vacío** (carga de 0 bytes) es válido y deja la tabla en blanco.
- Éxito: `ACK(0x03, 0)`. La nueva tabla reemplaza a la anterior y las tareas arrancan con `Start`. Con el nodo en ejecución se responde `ACK(0x03, 3)`.

## 8. AddTask (0x0A) — modo manual

Agrega una tarea sin detener el sistema. La carga es **una** línea con el mismo formato de `DeployTasks` (de 1 a 95 bytes, se tolera un `\n` final). El host ya hizo la admisión; el nodo solo valida formato, `id` único, tabla con menos de 8 tareas, una sola esporádica y que quepa en la cola de trabajos.

- Respuestas: `ACK(0x0A, 0)` aplicada, `ACK(0x0A, 1)` rechazada. No exige calibración previa.
- **En reposo:** se agrega al final de la tabla, sin evento.
- **En ejecución:** se aplica entre dos ticks y el ACK sale después de aplicarla. Las periódicas se liberan por primera vez en el siguiente tick. Se emite el evento `TASK_ADDED` con `tick` = tick de primera liberación, `id` y `arg` = ranura (1 a 8). El ACK y `TASK_ADDED` viajan por rutas distintas, así que el host no debe asumir cuál llega primero.
- Una esporádica agregada en marcha queda disponible de inmediato para el botón y para `SporadicRelease`.
- Las tareas agregadas se conservan tras `Stop`; solo un nuevo `DeployTasks` las borra. Las ranuras no se reutilizan (no hay baja de tareas).

## 9. Start y Stop

**Start (0x04).** Carga de 3 bytes: `[algoritmo][N: 2 B LE]`.

- `algoritmo`: 0 = RMS, 1 = EDF.
- `N`: período del TickReport en ticks; 0 lo desactiva.
- Exige haber calibrado. La tabla puede estar vacía.
- El **tick cero** es el instante en que el nodo termina de procesar la trama. Todos los contadores se reinician y el LED de plazo perdido se apaga.
- Los eventos del tick cero pueden llegar **antes** que el ACK de `Start`. El host debe despachar por tipo de trama, no por orden de llegada.
- `ACK(0x04, 3)` si ya está en ejecución o no hay calibración; `ACK(0x04, 1)` si la carga es inválida o no se pudo arrancar.
- **Desfase entre nodos.** El host inicia los nodos uno tras otro, así que hay un desfase de pocos milisegundos entre ellos. Además el reloj del nodo derivó −70 ppm respecto al del host en 10 minutos (unos 4 ms por minuto). Es una sola medición, no una cota; con un cristal distinto en cada nodo el desfase crece con el tiempo en ese orden de magnitud.

**Stop (0x08).** Carga vacía. El nodo termina el tramo en curso, detiene el temporizador, entrega todos los eventos pendientes y responde `ACK(0x08, 0)`. Después de ese ACK no llega ningún `Event` ni `TickReport`. La tabla se conserva y se puede volver a enviar `Start`.

- El ACK puede tardar hasta un tick más el vaciado de los eventos pendientes. Medido con tick de 1 ms: 9 a 10 ms con carga ligera y 338 ms con el búfer saturado. El host debe esperar al menos 1 tick + 1 s.
- El nodo no emite un evento de fin de ejecución: el Gantt termina en el último tick conocido.

**Si el host se detiene o se desconecta sin enviar `Stop`, el nodo sigue ejecutando sus tareas.** Se comprobó con un host que dejó de leer durante 8 s (al volver, `Stop` respondió normal y no hubo plazos ni ticks perdidos) y con un host que cerró el puerto (el nodo siguió alternando las tareas, observado en los LEDs).

## 10. Eventos (0x05)

Carga: `[cantidad: 1 B][registro × cantidad]`, hasta 56 registros por trama. Cada registro mide 9 bytes:

```
[tipo: 1 B][tick: 4 B LE][id: 2 B LE][arg: 2 B LE]
```

`tick` cuenta desde el tick cero de `Start`.

| Tipo | Nombre | `id` | `arg` |
|---|---|---|---|
| 0x01 | RELEASE | tarea | n.º de trabajo (16 bits bajos) |
| 0x02 | SWITCH | tarea que corre desde este tick (0 = ocioso) | tarea anterior (0 = ocioso) |
| 0x03 | COMPLETE | tarea | n.º de trabajo |
| 0x04 | MISS (trabajo abortado) | tarea | n.º de trabajo |
| 0x05 | SPORADIC_IGNORED | tarea esporádica (0 si no hay) | 0 = no pasó el intervalo mínimo, 1 = no hay esporádica |
| 0x06 | TASK_ADDED | tarea | ranura asignada |
| 0x07 | TICK_LOST | 0 | ticks no atendidos |
| 0x08 | BAD_FRAME | — | **No implementado**: las tramas malas solo se cuentan en el TickReport |

**Cómo reconstruir el Gantt.** Un SWITCH en el tick *k* significa que el tramo [*k*, *k*+1) pertenece a esa tarea hasta el siguiente SWITCH. COMPLETE lleva el tick en que termina el último tramo del trabajo. Las marcas `tick` son no decrecientes.

**Búfer.** Hay 256 registros en RAM y una tarea aparte los transmite; el calendarizador solo los encola. Si el búfer se llena, el nodo **descarta el evento nuevo** y lo cuenta en `eventos_descartados` (TickReport), así que el Gantt queda completo hasta ese punto. La capacidad sostenida medida es de ~1258 eventos/s, el límite de la UART a 115200 baudios. Con `N = 0` no hay aviso de descartes. `Start` vacía el búfer.

**TICK_LOST.** Se emite cuando el nodo no alcanzó a procesar uno o más ticks. `tick` es el tick en que se reanuda y `arg` la cantidad de ticks no atendidos. El tiempo lógico salta al tick actual (no se procesan atrasados en ráfaga): los trabajos cuyo plazo venció se abortan como MISS y cada tarea periódica se libera una sola vez. En las pruebas nunca ocurrió de forma natural, ni con sobrecarga a 1 ms; solo se provocó con un firmware de prueba (sección 16).

## 11. TickReport (0x06)

Se envía cada `N` ticks (el `N` de `Start`). Carga de 22 bytes, little endian:

```
[tick: 4][plazos_perdidos: 2][esporadicas_ignoradas: 2][ticks_perdidos: 2]
[eventos_descartados: 2][tramas_malas: 2][max_ocupado_us: 4][max_latencia_us: 4]
```

- `tick`: último tick procesado. El primer reporte sale en el tick `N`, luego `2N`, `3N`… Si se pierden ticks, el siguiente sale en el siguiente múltiplo de `N`, sin ráfaga.
- Los contadores son acumulados desde `Start`. Los de 16 bits se quedan en 65535 si se saturan.
- `plazos_perdidos` coincide con los MISS emitidos hasta ese tick. `esporadicas_ignoradas` coincide con los SPORADIC_IGNORED, salvo que por tick solo se emiten los primeros 8 eventos (el contador sí cuenta todas).
- `tramas_malas`: tramas del host descartadas por CRC, longitud o timeout. El ruido anterior a una marca de inicio no cuenta.
- `max_ocupado_us`: mayor tiempo de proceso dentro de un tick, incluyendo las UT. Dividido entre la duración del tick da la ocupación; como la calibración deja 80 % de margen, se espera algo cercano a eso (medido: 79 % a 86 %).
- `max_latencia_us`: mayor retraso entre la alarma del temporizador y el inicio del proceso del tick.

| Condición | `ut_por_tick` | `max_ocupado_us` | `max_latencia_us` |
|---|---|---|---|
| Tick de 10 ms, 10 min | 70 | 7914 (79 %) | 16 |
| Tick de 1 ms, carga ligera, 5 s | 7 | 806 (81 %) | 20 |
| Tick de 1 ms, sobrecarga, 12 s | 7 | 865 (86 %) | 16 |

Son mediciones, no cotas garantizadas. En la corrida de 10 minutos el máximo fue el mismo desde el primer minuto hasta el último.

**Reglas para el host:**

- `Event` y `TickReport` pueden llegar intercalados en cualquier orden. Un reporte puede llegar antes que eventos con `tick` menor o igual que aún estén en cola. Reconstruir siempre por el campo `tick`.
- Si un reporte no alcanza a salir antes de que se prepare el siguiente, se reemplaza (solo importa el último). Sigue saliendo aunque el búfer de eventos esté saturado.
- Para declarar un nodo caído, el host puede contar reportes consecutivos ausentes: cada reporte indica que el calendarizador sigue vivo. El intervalo entre reportes es `N × tick_us`.
- El campo `max_latencia_us` se agregó al final del formato original de 18 bytes. Se recomienda validar por longitud (22 bytes).

## 12. Calendarizador y tareas esporádicas

**Prioridad y desempate.** RMS usa el menor período `T` (prioridad fija); EDF usa el menor plazo absoluto. En ambos casos el desempate es: mayor criticidad primero y, si siguen empatadas, menor `id`. Es determinista: la misma carga produce siempre el mismo Gantt.

**Trabajos solapados.** Si un trabajo no terminó cuando llega la siguiente liberación (caso `D > T`), el nuevo se encola detrás, en orden FIFO. Cada trabajo conserva su propio plazo (de 64 bits), su trabajo restante y su número. Un trabajo solo se aborta cuando llega **su** plazo, nunca porque llegue el siguiente. Al abortarse se emite MISS, se enciende el LED de plazo perdido y los demás trabajos continúan.

**Tarea esporádica.** Se declara con tipo `S` (máximo una por nodo) y nunca se libera sola. `T` es el intervalo mínimo entre liberaciones y `D` su plazo; para la admisión el host la trata como periódica de período `T`. Hay dos fuentes de pulsación, con el mismo tratamiento:

- **El botón del nodo** en GPIO 27 (a GND, con pull-up interno). La rutina de interrupción solo registra la pulsación. Antirrebote: se acepta un flanco de bajada si pasaron al menos 50 ms desde el flanco anterior (de cualquier tipo), lo que también filtra el rebote al soltar. Dos pulsaciones separadas menos de 50 ms cuentan como una.
- **El mensaje `SporadicRelease` (0x07)**, con carga vacía. `ACK(0x07, 0)` significa que la pulsación quedó registrada (si libera o se ignora lo dicen los eventos). `ACK(0x07, 1)` si la carga no está vacía o no hay esporádica; `ACK(0x07, 3)` si el nodo no está en ejecución.

Las pulsaciones se procesan al inicio del siguiente tick, así que la liberación puede quedar hasta un tick después de la pulsación real. La primera siempre libera; las siguientes solo si pasaron al menos `T` ticks desde la última liberación aceptada. Si no, se emite SPORADIC_IGNORED (motivo 0) y se cuenta. Si hay varias pulsaciones en un mismo tick, solo la primera puede liberar. Sin esporádica en la tabla, el botón genera SPORADIC_IGNORED con `id` 0 y motivo 1. Las pulsaciones anteriores a `Start` se descartan.

**Latencia de respuesta.** El host la calcula con los eventos: RELEASE y COMPLETE de la esporádica. Medida con una esporádica de C = 2 y T = D = 50 ticks, la latencia fue de 2 a 3 ticks.

## 13. Hardware (LEDs y botón)

| Pin | Función |
|---|---|
| GPIO 18 a 22 (18, 19, 21, 22) | Tarea en ejecución, índice de ranura en binario; GPIO 18 es el bit 0 |
| GPIO 23 | Plazo perdido (se queda encendido hasta el siguiente `Start`) |
| GPIO 27 | Botón de la esporádica |

La ranura es la posición de la tarea en el despliegue (1 a 8; 0 = ocioso); `AddTask` usa la siguiente libre. Los eventos usan el `id` de la tarea, no la ranura. Si falla la inicialización del nodo se encienden los cinco LEDs.

## 14. Entradas inválidas

- Los 256 valores posibles de tipo, con carga vacía, respondieron el ACK esperado.
- `DeployTasks` y `AddTask` rechazan cargas de 512 bytes sin formato, líneas de más de 95 caracteres y tablas de más de 8 tareas.
- Con 400 entradas aleatorias (tramas con CRC válido y carga aleatoria, bits cambiados, ruido, longitudes excesivas y tramas truncadas) el nodo no se reinició, siguió respondiendo y ejecutó igual que el modelo.

## 15. Límites

| Límite | Valor |
|---|---|
| Carga útil máxima | 512 B |
| `tick_us` | 1000 a 1 000 000 |
| Tareas simultáneas | 8 |
| Línea de tarea | 95 caracteres |
| Esporádicas por nodo | 1 |
| `id` de tarea | 65535 |
| Registros de trabajo | 64 (`Σ(ceil(D/T)+1) ≤ 64`) |
| Búfer de eventos | 256 registros |
| Eventos por trama | 56 |
| Capacidad sostenida de eventos | ~1258/s |
| `N` del TickReport | 0 a 65535 |

## 16. Verificación

Las pruebas por cable usan un host de referencia en Python (`host_tests/`, con la opción `--port`; en Linux, por ejemplo `--port /dev/ttyUSB0`; requiere `pyserial` y el monitor serie cerrado). La clase `Node` de `test_start_stop.py` es una implementación de referencia del lado del host: armado de tramas, CRC-16 y despacho por tipo.

| Script | Qué verifica | Resultado |
|---|---|---|
| `test_start_stop.py` | Calibración, despliegue, Start, Stop y estados de ACK | 18 de 18 (EDF y RMS) |
| `test_events.py` | Flujo de eventos contra un modelo | 49 de 49 |
| `test_tickreport.py` | Período, contadores y tramas malas | 65 de 65 |
| `test_sporadic.py` | Esporádica con `SporadicRelease` y con botón real | 43 de 43; 57 de 57 con botón |
| `test_addtask.py` | `AddTask`, rechazos, persistencia | 103 de 103 |
| `test_stress.py` | Tick de 1 ms, sobrecarga, búfer lleno | 35 de 35 |
| `test_host_gone.py` | El nodo sigue sin host | 9 de 9, más observación por LEDs |
| `test_invalid_inputs.py` | 256 tipos, límites, parser, fuzz | 33 de 33 |
| `test_long_run.py` | 10 minutos, deriva del reloj | 17 de 17 |
| `test_tick_lost.py` | `TICK_LOST` (con firmware de prueba) | 15 de 15 |

Además hay pruebas en PC, sin placa, compiladas con `g++ -std=c++17`: `sched_core_test.cpp` (168 comprobaciones), `tick_report_test.cpp` (96), `task_parser_test.cpp` (38) y `event_queue_test.cpp`. Todas sin fallos.

Resultados destacados: en una corrida de 10 minutos (EDF, tick de 10 ms) se recibieron 60 011 ticks y 78 017 eventos idénticos al modelo, sin descartes ni `TICK_LOST`; 600 TickReport sin huecos; ocupación máxima de 79 % del tick.

**Alcance de estos resultados.** Las pruebas por cable se hicieron con una versión anterior del firmware. Después se cambiaron la cola de trabajos solapados (FIFO con plazos de 64 bits y 64 registros), la criticidad con signo y el despliegue vacío. Esos cambios están verificados con las pruebas nativas y con la compilación del SDK, pero no se volvieron a correr todas las pruebas por cable. El despliegue vacío en el ESP32 concretamente requiere comprobación con la placa.

**Lo que no se probó:**

- Desconectar físicamente el cable USB y reconectarlo (en la placa de desarrollo el mismo cable alimenta el nodo).
- Rebotes reales del botón de menos de 50 ms.
- El motivo 1 de SPORADIC_IGNORED.
- Latencia del ACK de `AddTask` y su orden respecto a `TASK_ADDED`.
- `AddTask` con tick de 1 ms.
- `Start` con tabla vacía.
- Tick de 1 s y corridas de más de 10 minutos.
- El LED de ranura de una tarea agregada en marcha.

`TICK_LOST` y `ticks_perdidos` distinto de 0 solo se observaron con un firmware de prueba que bloquea el calendarizador a propósito (entorno `esp32dev_fault`, bandera `RT_FAULT_INJECT`). Ese firmware usa una espera activa para provocar la falla y no forma parte de lo que se entrega.

## 17. Uso de FreeRTOS

**Qué dice el enunciado.** No se permite usar `delay()`, `sleep` ni espera activa como mecanismo de temporización, ni delegar en un RTOS la decisión del orden de ejecución (prioridades, colas o retardos). Se permiten bibliotecas externas, excepto las que implementen la calendarización.

**Por qué FreeRTOS no incumple eso.**

1. **El orden de ejecución lo decide el calendarizador propio implementado en código.** Las tareas del sistema no son tareas de FreeRTOS: son filas de una tabla propia y sus trabajos viven en una cola propia, ambas en `sched_core.cpp`. Ahí se implementan RMS (período fijo), EDF (plazo absoluto), el desempate, las liberaciones, el aborto por plazo vencido y los cambios de tarea. FreeRTOS nunca ve los períodos, plazos ni prioridades de las tareas del usuario.
2. **El núcleo del calendarizador no depende de FreeRTOS.** `sched_core.cpp` no incluye ninguna cabecera de FreeRTOS ni de hardware, y por eso se prueba en un PC con `g++`.
3. **La temporización sale de un temporizador de hardware.** El tick lo genera un `gptimer` a 1 MHz con recarga automática. Su interrupción solo incrementa un contador y despierta al hilo calendarizador. No usamos `vTaskDelay`, `vTaskDelayUntil`, `delay()`, `sleep` ni espera activa para medir el tiempo.
4. **No se usan las colas ni los retardos de FreeRTOS para planificar.** El búfer de eventos es un anillo propio (`event_queue.cpp`). Las esperas son bloqueos sin consumo de CPU (`xTaskNotifyWait`, semáforos y mutex con `portMAX_DELAY`). El único timeout con tiempo es de 100 ms entre bytes de la UART, para descartar una trama a medias; no tiene relación con la ejecución de las tareas.
5. **Las prioridades de FreeRTOS son fijas y de infraestructura.** Solo hay tres hilos: el calendarizador (prioridad 10, núcleo 1), la transmisión de eventos (prioridad 2, núcleo 0) y `app_main` con la UART (prioridad 1, núcleo 0). Sirven para que un tick nunca se retrase por la comunicación. No cambian en ejecución y no dependen de las tareas del usuario.
6. **Dos aclaraciones sobre espera activa.** El ciclo de calibración ejecuta UT mientras cuenta ticks del temporizador: es la medición, no una espera. La única espera activa del código (`esp_rom_delay_us`) está bajo `RT_FAULT_INJECT`, solo para la prueba de `TICK_LOST`.

**Por qué fue necesario.** El ESP32 se programa con ESP-IDF, que arranca sobre FreeRTOS: `app_main` ya es una tarea, y el driver de la UART, la interrupción del temporizador y el servicio de interrupciones de GPIO se apoyan en él. Quitarlo implicaría programar el ESP32 sin ESP-IDF, reescribiendo esos drivers, y ese trabajo no aporta nada al objetivo del proyecto. Además necesitábamos primitivas que FreeRTOS da hechas y que permiten no usar espera activa:

- Pasar el aviso de la interrupción del temporizador al hilo calendarizador (notificaciones de tarea).
- Un mutex para que las tramas salientes de dos hilos no se mezclen.
- Un semáforo para que `Stop` espere a que se vacíen los eventos.
- Fijar el calendarizador a su propio núcleo, aislado de la UART, que es lo que mantiene estable el tick mientras se transmite.

En resumen, FreeRTOS es la capa del sistema sobre la que corre el firmware, y la política de calendarización de tiempo real (qué trabajo avanza en cada tick) es nuestra.