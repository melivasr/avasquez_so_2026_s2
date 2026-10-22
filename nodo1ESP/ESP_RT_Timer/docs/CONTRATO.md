# README_NODO — Nodo ESP32 (contrato serial para el host)

Documento para quienes desarrollan el host. El contrato entre host y nodo es **solo** el protocolo serial descrito aquí; no hace falta conocer el interior del nodo.

## Leyenda de estados

| Etiqueta | Significado |
|---|---|
| **[probado por cable]** | Verificado con un host de prueba en Python contra un ESP32 real. |
| **[implementado, sin probar por cable]** | Existe en el código del nodo; sin evidencia por cable. |
| **[propuesto — pendiente de acordar con el host]** | Solo diseño. No existe en el nodo. No programar contra esto hasta acordarlo. |

Versión del documento: borrador 10 (AddTask implementado, sin probar por cable).

---

## 1. Enlace físico

- UART0 del ESP32 vía USB-serial, 115200 baudios, 8N1, sin control de flujo. **[probado por cable]**
- UART0 es canal **exclusivo** del protocolo: el nodo no imprime texto de depuración (consola desactivada). Todo byte que salga del nodo, salvo el ruido de arranque (sección 6), es una trama. **[probado por cable]**
- Puerto en el equipo de desarrollo del nodo: `/dev/cu.usbserial-0001` (macOS). En Linux el nombre cambia (p. ej. `/dev/ttyUSB0`).

## 2. Formato de trama

```
[A5 5A][tipo: 1 B][longitud: 2 B LE][carga útil: longitud B][CRC-16: 2 B LE]
```

- Marca de inicio: `0xA5 0x5A`. **[probado por cable]**
- CRC-16 CCITT-FALSE (polinomio 0x1021, valor inicial 0xFFFF, sin XOR final), calculado sobre **tipo + longitud + carga útil** (no incluye la marca de inicio). **[probado por cable]**
- Carga útil máxima: 512 bytes. Tamaño máximo de trama: 519 bytes. **[probado por cable]**
- Trama con CRC incorrecto: el nodo la descarta **sin responder**. **[probado por cable]**
- Trama truncada seguida de otra completa: el nodo se resincroniza (timeout entre bytes de 100 ms). **[probado por cable]**
- Reporte de tramas corruptas al host: **no implementado** (ver sección 9).

Ejemplo (ACK de un despliegue aceptado): `a5 5a 09 02 00 03 00 4b 01`

## 3. Tipos de mensaje

| Tipo | Valor | Dirección | Estado |
|---|---|---|---|
| CalibrateReq | 0x01 | host → nodo | probado por cable |
| CalibrateResp | 0x02 | nodo → host | probado por cable |
| DeployTasks | 0x03 | host → nodo | probado por cable |
| Start | 0x04 | host → nodo | probado por cable a nivel de ACK (con `N = 0`). Ver 9.1 |
| Event | 0x05 | nodo → host | probado por cable (tipos 0x01 a 0x04); 0x07 implementado, sin provocar. Ver 9.5 |
| TickReport | 0x06 | nodo → host | probado por cable. Ver 9.6 |
| SporadicRelease | 0x07 | host → nodo | probado por cable. Ver 9.7 |
| Stop | 0x08 | host → nodo | probado por cable a nivel de ACK. Ver 9.2 |
| Ack | 0x09 | nodo → host | probado por cable |
| AddTask | 0x0A | host → nodo | propuesto (sección 9) |

Un tipo desconocido o aún no implementado recibe `ACK(tipo, 2)`.

## 4. ACK (0x09) — nodo → host

Carga útil de 2 bytes: `[tipo original: 1 B][estado: 1 B]`. **[probado por cable]**

| Estado | Significado | Estado del documento |
|---|---|---|
| 0 | Aceptado | probado por cable |
| 1 | Rechazado (datos inválidos) | probado por cable |
| 2 | No implementado | probado por cable (con `Start`) |
| 3 | Estado inválido para esa operación | probado por cable; formato pendiente de acordar con el host |

Usos del estado 3 **[probado por cable]**: `Start` con el nodo ya en ejecución o sin calibración previa; `Stop` sin ejecución; `DeployTasks` o `CalibrateReq` con el nodo en ejecución.

## 5. Calibración

**CalibrateReq (0x01), host → nodo.** Carga de 4 bytes: `tick_us` uint32 LE. Rango válido: 1000 a 1 000 000 (1 ms a 1 s). **[probado por cable]**

**CalibrateResp (0x02), nodo → host.** Carga de 4 bytes: `ut_por_tick` uint32 LE (el `sn` del enunciado). **[probado por cable]**

- Medición: ventana de 50 ticks, margen del 80 %. Resultado observado: **70 UT/tick con tick de 10 ms**, idéntico en 5 de 5 ejecuciones. **[probado por cable]**
- El valor 70 **no está validado** con una medición independiente.
- Duración: ~0,50 s por calibración (medido). **Durante la calibración el nodo no atiende nada más.** El host debe esperar la respuesta con un temporizador holgado (predicción: 2 s es suficiente; no medido como cota). **[probado por cable]**
- Error: `ACK(0x01, 1)` si la longitud ≠ 4, si `tick_us` está fuera de rango (probado con `tick_us = 5`) o si la medición falla. **[probado por cable]**
- La calibración debe repetirse en cada ejecución del sistema; el nodo no guarda el valor entre reinicios.
- La medición corre en el núcleo del calendarizador. Con el nodo en ejecución (después de `Start`), `CalibrateReq` responde `ACK(0x01, 3)`. El `tick_us` de la última calibración exitosa es el tick con el que se ejecuta `Start`. **[probado por cable: estado 3 en ejecución; calibración con tick de 200 ms (~10 s) respondió `ut_por_tick = 1417` en 2 de 2 ejecuciones, medida en el núcleo del calendarizador]**
- La duración es ~50 ticks (con tick de 10 ms, ~0,5 s; con tick de 1 s, ~50 s). El host debe esperar la respuesta con un temporizador acorde.

## 6. Comportamiento del puerto serial al abrirlo (importante para el host)

- Abrir el puerto desde Python **reinicia la placa**, incluso con `dtr=False` y `rts=False` asignados antes de `open()`. **[observado]**
- Tras el reinicio sale texto de arranque de la ROM (observado: exactamente 304 bytes en 8 de 8 aperturas; 1959 bytes tras un Upload, sin inspeccionar). El host debe **vaciar el ruido hasta observar ~0,5 s de silencio** y resincronizar por `A5 5A`. Un `read(512)` fijo para vaciar **falló**. **[observado]**
- Abrir con DTR/RTS por defecto a veces dejó al nodo sin responder; **causa no confirmada**.
- Problema abierto sin explicar: dos veces el primer ACK llegó con el encabezado dañado, ambas tras un Upload; no se reprodujo después. **Requisito para el host: tratar "sin ACK válido" como situación normal** (temporizador, reintento, resincronización por `A5 5A`).

## 7. DeployTasks (0x03) — host → nodo

Carga útil: texto ASCII, una tarea por línea, líneas separadas **solo con `\n`** (con `\r\n` las líneas se rechazan). **[probado por cable]**

```
id,tipo,W,T,D,criticidad
3,P,40,20,20,2
```

- `tipo`: `P` (periódica) o `S` (esporádica). `id`, `W`, `T`, `D` > 0. Todos los campos son enteros decimales sin signo (sin espacios). `T` y `D` en ticks; `W` en UT.
- **Todo o nada**: si una línea es inválida, no se despliega nada y se responde `ACK(0x03, 1)`; la tabla anterior queda intacta. **[probado por cable]** con tipo inválido e ID duplicado. Las demás validaciones (campos faltantes o sobrantes, longitud de línea ≤ 95, no numéricos) **[probado solo en memoria]**.
- Máximo 8 tareas por despliegue (`MAX_TASKS = 8`). **[implementado, sin probar por cable]**
- Éxito: `ACK(0x03, 0)`. La tabla reemplaza la anterior; las tareas empiezan a ejecutarse con `Start`.
- Se rechaza (`ACK(0x03, 1)`) un `id` > 65535 o una segunda tarea esporádica en el mismo despliegue. **[probado por cable]**
- Con el nodo en ejecución se responde `ACK(0x03, 3)`. **[probado por cable]** Que la tabla no cambia no se verificó.

## 8. Secuencia de uso

Verificada por cable hasta el paso 4:

1. Abrir el puerto (la placa se reinicia).
2. Vaciar el ruido de arranque hasta ~0,5 s de silencio.
3. `CalibrateReq(tick_us)` → `CalibrateResp(ut_por_tick)`.
4. `DeployTasks(líneas)` → `ACK(0x03, 0)`.

Probado por cable a nivel de ACK (9.1 y 9.2):

5. `Start` → `ACK(0x04, 0)`; el nodo ejecuta las tareas y empieza a enviar `Event` (probado por cable) y, si `N` > 0, `TickReport` (probado por cable). Los primeros `Event` pueden llegar antes que el ACK de `Start`.
7. `Stop` → `ACK(0x08, 0)`.

Propuesto, no existe en el nodo:

6. `SporadicRelease` (probado por cable; ver 9.7) o el botón del nodo (probado, ver 9.7). `TickReport` se emite entre los pasos 5 y 7.
6b. (Modo manual) `AddTask`: propuesto, no existe en el nodo.

## 9. Propuestas pendientes de acordar con el host

> Cada subsección indica su estado. Lo que no está marcado como implementado es **[propuesto — pendiente de acordar con el host]** y no existe en el nodo. Todo formato nuevo está pendiente de acordar con el host aunque ya esté implementado y probado.

### 9.1 Start (0x04), host → nodo — [probado por cable]

Probado por cable: `Start` válido → estado 0; carga de 2 bytes → 1; algoritmo 2 → 1; repetido → 3; sin calibrar → 3; `Start` tras `Stop` → 0; `Start` con `N` = 0, 1, 10, 20 y 100; ejecución con tick de 1 ms, 10 ms y 200 ms. **No probado por cable:** `Start` con la tabla vacía. La ejecución de las tareas se observó además por LEDs en EDF y RMS, congruente con lo esperado (observación del desarrollador del nodo).

Carga de 3 bytes: `[algoritmo: 1 B][N: 2 B LE]`

- `algoritmo`: 0 = RMS, 1 = EDF.
- `N`: periodo del `TickReport` en ticks; 0 desactiva los reportes. El nodo acepta cualquier valor (0 a 65535). Ver 9.6.
- Prerrequisito: haber calibrado. El tick de ejecución es el `tick_us` de la última calibración exitosa (`Start` no lo lleva). La tabla puede estar vacía (un nodo sin tareas asignadas igual puede iniciarse).
- Efecto: el **tick cero** es el instante en que el nodo termina de procesar la trama; luego envía `ACK(0x04, 0)`. Todos los contadores se reinician.
- `ACK(0x04, 3)` si ya está en ejecución o no hubo calibración. `ACK(0x04, 1)` si la longitud de la carga ≠ 3, el algoritmo no es 0 ni 1, o el nodo no pudo iniciar la ejecución (tabla inválida o fallo del temporizador).
- El ACK se envía justo después de iniciar el temporizador; el tick cero se procesa a continuación. Los `Event` del tick cero pueden llegar **antes** que el ACK de `Start`: el host debe despachar las tramas por tipo, no por orden de llegada.
- El LED de plazo perdido se apaga en cada `Start`.
- **Desfase entre nodos.** El host inicia los nodos en secuencia, así que habrá un desfase de pocos ms por nodo. Cómo medirlo está por acordar. Dato medido: el reloj del nodo derivó **−70 ppm** respecto al del host en una corrida de 10 minutos (unos 4 ms por minuto; −27 ppm en una corrida de 1 minuto, más sensible al jitter de llegada). Es una sola medición, no una cota. Con un cristal distinto en cada nodo, el desfase entre nodos crece con el tiempo en ese orden de magnitud.

### 9.2 Stop (0x08), host → nodo — [probado por cable]

Probado por cable: `Stop` en ejecución → 0; repetido o sin ejecución → 3; con carga → 1.

Carga vacía (otra longitud → `ACK(0x08, 1)`). El nodo termina el tramo en curso, detiene el temporizador y envía `ACK(0x08, 0)`. `ACK(0x08, 3)` si no estaba en ejecución. Vuelve a estado inactivo (la tabla se conserva; un nuevo `DeployTasks` la reemplaza y se puede volver a enviar `Start`). Antes de enviar el ACK, el nodo entrega todos los `Event` pendientes: después del ACK de `Stop` no llegan más `Event` ni `TickReport`. **[probado por cable]** El nodo no emite un evento de fin: el Gantt termina en el último tick conocido (punto abierto, ver 9.5 y 9.11).

**Latencia del ACK.** Puede tardar hasta un tick más el vaciado de los eventos pendientes. Medido por cable con tick de 1 ms: 9 a 10 ms con carga ligera y **338 ms** con el búfer de eventos saturado (hay que vaciar hasta 256 registros por la UART a 115200 baudios). Recomendación para el host (derivada, no medida con ticks largos): esperar el ACK durante al menos 1 tick + 1 s.

**Host que se detiene o desconecta sin enviar `Stop`:** el nodo sigue ejecutando sus tareas. **[probado por cable]**

- Host que deja de leer (puerto abierto, 8 s, tick de 10 ms, EDF): tras volver a leer, `Stop` responde `ACK(0x08, 0)`, el tick del último `TickReport` corresponde al tiempo transcurrido (el nodo no se detuvo), no hay plazos ni ticks perdidos y el flujo de eventos posterior a la pausa coincide con el modelo.
- Host que termina sin enviar `Stop` (el puerto se cierra, tick de 200 ms, RMS): observado por LEDs, el nodo siguió alternando las ranuras y el LED de plazo perdido siguió encendido.
- **No probado:** desconectar físicamente el cable USB (en la placa de desarrollo el mismo cable alimenta el nodo, así que requiere alimentarlo aparte) y volver a conectarlo.

### 9.3 DeployTasks mientras hay ejecución — [probado por cable]

Se rechaza con `ACK(0x03, 3)`. Para agregar tareas en marcha se usa `AddTask`.

**Formato de la línea de `DeployTasks`.** El mismo formato estricto de 9.4. Una carga con un byte NUL se rechaza completa con `ACK(0x03, 1)`; no se aplica ninguna tarea. **[probado por cable]**

### 9.4 AddTask (0x0A), host → nodo (modo manual) — [probado por cable; formato pendiente de acordar con el host]

Carga: **una** línea de texto con el formato de `DeployTasks` (`id,tipo,W,T,D,criticidad`), de 1 a 95 bytes; se tolera un `\n` final. Válido en reposo y en ejecución. El host ya hizo la admisión; el nodo solo valida formato, `id` ≤ 65535, `id` único, tabla con menos de 8 tareas y una sola esporádica. Una carga vacía, demasiado larga o con byte NUL se rechaza.

**Formato estricto de la línea:** exactamente 6 campos separados por **una sola** coma. Se rechazan con `ACK(0x0A, 1)`: campos vacíos (coma doble), coma al inicio o al final, espacios, signos, `\r` final, tipo en minúscula, campos extra y números de más de 32 bits. Es la misma regla que en `DeployTasks`.

- **Respuestas:** `ACK(0x0A, 0)` aplicada; `ACK(0x0A, 1)` rechazada. El estado `3` por falta de calibración **ya no existe**: el nodo no exige calibración aquí.
- **En reposo:** se agrega al final de la tabla, sin evento. Un `DeployTasks` posterior reemplaza toda la tabla.
- **En ejecución:** se aplica entre dos ticks y el ACK sale **después** de aplicarla. En teoría espera como máximo el tick en curso; la latencia no se ha medido por cable. Las periódicas se liberan por primera vez en el siguiente tick a procesar. Se emite `TASK_ADDED` (9.5) con `tick` = tick de primera liberación, `id` y `arg` = ranura 1..8. El ACK y `TASK_ADDED` viajan por rutas distintas: el host no debe asumir cuál llega primero.
- **Esporádica agregada en marcha:** `tick` = tick en que se aplicó (no tiene primera liberación propia); desde ese momento `SporadicRelease` y el botón la usan.
- **Persistencia:** las tareas agregadas se conservan tras `Stop`; un `Start` posterior las ejecuta. Solo un nuevo `DeployTasks` las borra.
- Las ranuras no se reutilizan (no hay baja de tareas).

Probado por cable (tick de 10 ms, EDF y RMS, `test_addtask.py`: 103 de 103): rechazos con `ACK(0x0A, 1)` de `id` repetido, carga vacía, solo `\n`, 200 B, NUL embebido, basura, `id` > 65535, tipo inválido, `W` = 0 y campos faltantes, sin alterar la ejecución; aceptación en ejecución con flujo de eventos idéntico al modelo; persistencia tras `Stop`/`Start`; aceptación en reposo; octava tarea aceptada y novena rechazada; esporádica agregada en marcha (la segunda esporádica se rechaza y `SporadicRelease` pasa a registrarse). Además, `test_invalid_inputs.py` (33 de 33): línea de 95 caracteres aceptada y de 96 rechazada (con y sin `\n` final) y rechazo de comas dobles, iniciales y finales, espacios, signos, `\r`, campos extra, tipo en minúscula y números de 11 dígitos.

**No probado:** `AddTask` con tick de 1 ms, orden relativo entre el ACK y `TASK_ADDED`, latencia del ACK.

### 9.5 Event (0x05), nodo → host — [RELEASE, SWITCH, COMPLETE, MISS, TASK_ADDED, SPORADIC_IGNORED y TICK_LOST probados por cable]

Probado por cable:

- **Flujo contra el modelo** (conjuntos A y B, RMS y EDF, tick de 10 ms, 300 ticks, `test_events.py`, 49 de 49): RELEASE, SWITCH, COMPLETE y MISS coinciden evento por evento con un modelo del calendarizador; marcas de tiempo no decrecientes; tramas bien formadas; sin TICK_LOST; ningún `Event` después del ACK de `Stop`.
- **Corrida larga** (EDF, tick de 10 ms, 10 minutos, `test_long_run.py`): **60 011 ticks y 78 017 eventos idénticos al modelo** de principio a fin, sin descartes ni TICK_LOST.
- **Tick de 1 ms** (carga ligera, 5 s, `test_stress.py`): flujo idéntico al modelo en 1000 ticks, sin descartes.
- **Búfer lleno** (sobrecarga a 1 ms: 8 tareas con `T = D = 1`, unos 16 eventos por tick, 12 s): el nodo descartó los eventos nuevos y conservó los más antiguos (el primer lote recibido es el del tick 0), las marcas `tick` siguieron siendo no decrecientes, las tramas llegaron bien formadas y `eventos_descartados` saturó en 65535.
- **TICK_LOST** (`test_tick_lost.py`, 15 de 15, con un firmware de prueba que bloquea el calendarizador; tick de 10 ms, EDF): llegaron exactamente `TICK_LOST` con `tick` 153 y `arg` 2, y con `tick` 620 y `arg` 39; el flujo de 1255 eventos coincidió con el modelo con saltos de tiempo.
- **SPORADIC_IGNORED** con motivo 0 (vía `SporadicRelease` y con el botón real), ver 9.7.

**Capacidad medida:** unos **1258 eventos por segundo**, el límite de la UART a 115200 baudios. Una carga que produzca más que eso de forma sostenida pierde eventos; el host debe consultar `eventos_descartados` (9.6) para saber si el Gantt está completo.

**TICK_LOST.** Se emite cuando el nodo no alcanzó a procesar uno o más ticks. `tick` = tick en que se reanuda el procesamiento y `arg` = cantidad de ticks no atendidos. El tiempo lógico salta al tick actual (no se procesan ticks atrasados en ráfaga): al reanudar, los trabajos cuyo plazo venció se abortan como MISS (se observó un MISS en el segundo bloqueo) y cada tarea periódica se libera una sola vez. **TICK_LOST nunca ocurrió de forma natural** en ninguna prueba, ni con sobrecarga a 1 ms; solo se provocó con un firmware de prueba. El firmware entregado no contiene esa inyección.

**No probado:** el saturado de `arg` en 65535 (más de 65 s sin atender ticks; lectura del código); el motivo 1 de SPORADIC_IGNORED (sin tarea esporádica, solo ocurre con el botón); corridas de más de 10 minutos. **No implementado:** BAD_FRAME (decisión pendiente con el host, ver 9.11).

Comportamiento del búfer: 256 registros en RAM, transmitidos en segundo plano por una tarea aparte; el calendarizador solo encola. Si el búfer se llena se **descarta el evento nuevo** y se cuenta; el host se entera por el contador `eventos_descartados` del `TickReport` (9.6); con `N` = 0 no hay aviso. El `Start` vacía el búfer. Orden: las marcas `tick` son no decrecientes; COMPLETE se estampa con el tick de fin del tramo. Punto abierto para acordar con el host: al hacer `Stop` no se emite un evento de fin de ejecución; si el host lo necesita para cerrar el Gantt, se propone agregarlo.

Carga: `[cantidad: 1 B][registro × cantidad]`, hasta 56 registros por trama. Registro de 9 bytes:

```
[tipo: 1 B][tick: 4 B LE][id: 2 B LE][arg: 2 B LE]
```

`tick` = ticks desde el tick cero de `Start`.

| Tipo | Nombre | `id` | `arg` |
|---|---|---|---|
| 0x01 | RELEASE | tarea | n.º de trabajo (16 bits bajos) |
| 0x02 | SWITCH | tarea que corre desde este tick (0 = ocioso) | tarea anterior (0 = ocioso) |
| 0x03 | COMPLETE | tarea | n.º de trabajo |
| 0x04 | MISS (trabajo abortado) | tarea | n.º de trabajo |
| 0x05 | SPORADIC_IGNORED (pulsación ignorada) | tarea esporádica (0 si no hay) | motivo: 0 = no pasó el intervalo mínimo, 1 = no hay tarea esporádica |
| 0x06 | TASK_ADDED | tarea | ranura asignada |
| 0x07 | TICK_LOST | 0 | ticks no atendidos |
| 0x08 | BAD_FRAME (máx. una vez por tick) — **no implementado** | 0 | total acumulado |

Convención: un SWITCH en el tick *k* significa que el tramo [*k*, *k*+1) pertenece a esa tarea hasta el siguiente SWITCH. COMPLETE se estampa con el tick en que termina el último tramo del trabajo. Con esto el host reconstruye el Gantt.

Límite que esto introduce: **`id` ≤ 65535** (el nodo rechaza IDs mayores).

### 9.6 TickReport (0x06), nodo → host — [probado por cable]

Probado por cable:

- **Tick de 10 ms:** periodo `N` = 20, 10, 1 y 0 (sin huecos entre reportes; con `N` = 1, 205 reportes consecutivos en ~2 s); contadores no decrecientes; `plazos_perdidos` igual a los MISS recibidos hasta cada tick; `tramas_malas` exacto (5 tramas con CRC malo + 1 truncada = 6) sin alterar la ejecución; ningún reporte después del ACK de `Stop`.
- **Corrida larga** (10 minutos, `N` = 100): 600 reportes cada 100 ticks sin huecos, desde el tick 100; sin plazos perdidos, ticks perdidos, descartes ni tramas malas.
- **Tick de 1 ms** (`N` = 100, 5000 ticks): reportes cada 100 ticks sin huecos y sin pérdidas con carga ligera.
- **Sobrecarga:** `plazos_perdidos` y `eventos_descartados` **saturan en 65535**; `ticks_perdidos` sigue en 0; los reportes siguen saliendo durante la sobrecarga (`test_stress.py`, 35 de 35).
- **Ticks perdidos** (firmware de prueba con bloqueos forzados, `test_tick_lost.py`, `N` = 100): reportes en los ticks 100, 200, 300, 400, 500, **620** y 700 (el reporte del tick 600 se saltó y salió uno solo en el 620, sin ráfaga); `ticks_perdidos` acumulado de 0, 2 y 41 en los reportes correspondientes; `plazos_perdidos` igual a los MISS recibidos hasta cada tick.

**No probado:** `esporadicas_ignoradas` con tick de 1 ms.

**Medición del margen de calibración** (no son cotas garantizadas):

| Condición | `ut_por_tick` | `max_ocupado_us` | `max_latencia_us` |
|---|---|---|---|
| Tick de 10 ms, carga B, corrida de 10 min | 70 | 7914 µs (79 %) | 16 µs |
| Tick de 1 ms, carga ligera, 5 s | 7 | 806 µs (81 %) | 20 µs |
| Tick de 1 ms, sobrecarga, 12 s | 7 | 865 µs (86 %) | 16 µs |

En la corrida de 10 minutos el máximo de ocupación fue el mismo desde el primer minuto hasta el último (no creció con el tiempo).

> **Cambio respecto a la propuesta anterior (18 bytes):** se agregó `max_latencia_us` al final. Un parser que lea solo los primeros 18 bytes sigue funcionando; se recomienda validar por longitud (22 bytes).

Se envía cada `N` ticks (el `N` de `Start`; 0 = no se envía). Carga de 22 bytes, little endian:

```
[tick: 4 B][plazos_perdidos: 2 B][esporadicas_ignoradas: 2 B][ticks_perdidos: 2 B]
[eventos_descartados: 2 B][tramas_malas: 2 B][max_ocupado_us: 4 B][max_latencia_us: 4 B]
```

- **`tick`:** último tick procesado. El primer reporte sale en el tick `N`, luego en `2N`, `3N`... Si se pierden ticks, el reporte siguiente sale en el siguiente múltiplo de `N` (no hay ráfagas). **[probado por cable]**
- **Contadores:** acumulados desde `Start`. Los de 16 bits saturan en 65535.
- **`plazos_perdidos`:** coincide con la cantidad de eventos MISS hasta ese `tick`.
- **`esporadicas_ignoradas`:** pulsaciones de la esporádica ignoradas (intervalo mínimo no cumplido o sin tarea esporádica); coincide con los eventos SPORADIC_IGNORED hasta ese `tick`, salvo que por tick solo se emiten los primeros 8 eventos de ignoradas (el contador sí cuenta todas). **[probado por cable]**
- **`ticks_perdidos`:** ticks que el nodo no alcanzó a procesar. **[probado por cable con un firmware de prueba]**
- **`eventos_descartados`:** eventos perdidos por búfer lleno (9.5).
- **`tramas_malas`:** tramas recibidas del host y descartadas por CRC o longitud inválidos, por exceder el tamaño máximo o por quedar incompletas (timeout entre bytes). El ruido antes de una marca de inicio no cuenta.
- **`max_ocupado_us`:** mayor tiempo de proceso dentro de un tick (incluye ejecutar las UT del tramo). Dividido entre la duración del tick indica la ocupación; la calibración deja un margen del 80 %, por lo que se espera un valor cercano a ese porcentaje (medido: 79 % a 86 %).
- **`max_latencia_us`:** mayor retraso entre la alarma del temporizador y el inicio del proceso del tick. Tras ticks perdidos refleja ese retraso: en la prueba con bloqueos forzados llegó a 8441 µs (con un tick de 10 ms).

Reglas de uso para el host:

- `Event` y `TickReport` pueden intercalarse en **cualquier orden**: un reporte puede llegar antes que eventos con `tick` menor o igual al suyo que aún estén en cola, y los eventos de un tick posterior pueden llegar antes que el reporte de un tick anterior. No asumir que, al recibir un reporte, ya llegaron todos los eventos hasta ese tick; reconstruir siempre por el campo `tick`.
- Se transmite en segundo plano con prioridad baja. Si un reporte no alcanza a salir antes de que se prepare el siguiente, se reemplaza (solo importa el más reciente); con `N` = 1 no se observaron reemplazos.
- El reporte sale entre lotes de eventos, de modo que **sigue saliendo aunque el búfer de eventos esté saturado**. Probado por cable con 12 s de sobrecarga a 1 ms.
- Tras el ACK de `Stop` no llega ningún `TickReport` más.
- Para declarar un nodo caído, el host puede contar reportes consecutivos ausentes (cada reporte indica que el calendarizador sigue vivo). El intervalo entre reportes es `N × tick_us`.

### 9.7 Tarea esporádica, botón y SporadicRelease (0x07) — [probado por cable]

Probado por cable (tick de 10 ms, EDF y RMS, tarea esporádica con C = 2, T = D = 50 ticks junto a una periódica): la esporádica no se libera sola; tres `SporadicRelease` producen 2 liberaciones y 1 ignorada (motivo 0), separadas al menos `T`; números de trabajo consecutivos; cada liberación tiene su COMPLETE; latencia de respuesta de 2 a 3 ticks (medida liberación → fin, con la periódica de mayor prioridad); sin plazos perdidos; `ACK(0x07, 3)` sin ejecución, `ACK(0x07, 1)` sin esporádica o con carga no vacía. Con el botón real (GPIO 27): 6 pulsaciones lentas produjeron exactamente 6 liberaciones, ninguna ignorada y ningún rebote contado (incluido el rebote al soltar); latencias de 2 a 3 ticks. Segunda prueba con el botón real (20 s, 15 pulsaciones entre lentas y rápidas): 9 liberaciones y 6 ignoradas (motivo 0) = 15 pulsaciones; cada ignorada cayó a menos de `T` ticks de una liberación; liberaciones separadas al menos `T`; ningún rebote contado; latencias de 2 a 3 ticks; `esporadicas_ignoradas` coincidió con los eventos. **No probado por cable:** pulsaciones separadas menos de 50 ms (antirrebote con rebotes reales), el motivo 1 de SPORADIC_IGNORED, dos pulsaciones en un mismo tick con el botón y `D` > `T` con la esporádica (solo en pruebas aisladas).

**Tarea esporádica.** Se declara en `DeployTasks` con tipo `S` (máximo una por nodo). Nunca se libera sola. `T` es el intervalo mínimo entre liberaciones y `D` su plazo; para la admisión el host la trata como periódica con periodo `T`.

**Fuentes de pulsación.** Dos, con el mismo tratamiento:

- El botón del nodo en **GPIO 27** (a GND, con pull-up interno). La rutina de interrupción solo registra la pulsación. Antirrebote: se acepta un flanco de bajada si pasaron al menos **50 ms** desde el flanco anterior (de cualquier tipo), lo que filtra el rebote al soltar. Dos pulsaciones separadas menos de 50 ms cuentan como una.
- El mensaje `SporadicRelease` (carga vacía, host → nodo).

**Procesamiento.** Las pulsaciones se procesan al inicio del siguiente tick: la liberación queda estampada con ese tick (hasta un tick de retardo respecto a la pulsación real). La primera pulsación siempre libera; las siguientes liberan solo si pasaron al menos `T` ticks desde la **última liberación aceptada**. Si no, se emite SPORADIC_IGNORED (motivo 0) y se cuenta. Si hay más de una pulsación en un mismo tick, solo la primera puede liberar. Sin tarea esporádica en la tabla, el botón genera SPORADIC_IGNORED con `id` = 0 y motivo 1. Las pulsaciones anteriores al `Start` se descartan. Si la liberación llega con el trabajo anterior aún activo (`D` > `T`), se aborta como plazo perdido, igual que en una periódica.

**Latencia de respuesta.** Se obtiene en el host de los eventos: RELEASE (liberación) y COMPLETE (fin) de la tarea esporádica.

**SporadicRelease.** `ACK(0x07, 0)`: la pulsación quedó **registrada** (si libera o se ignora lo informan los eventos). `ACK(0x07, 1)`: carga no vacía, o el nodo no tiene tarea esporádica. `ACK(0x07, 3)`: el nodo no está en ejecución.

### 9.8 Mapeo de LEDs — [implementado; probado con una demostración, no con el protocolo]

Verificado por el desarrollador del nodo en una demostración con tareas fijas (RMS y EDF, patrón de LEDs y LED de plazo perdido según lo predicho). GPIO 18 = bit 0 (menos significativo) a GPIO 22 = bit 3; GPIO 23 = plazo perdido (enclavado hasta el siguiente `Start`).

Ranura = posición de la tarea en el despliegue (1 a 8); `AddTask` usa la siguiente ranura libre. Los 4 LEDs muestran el índice de ranura en binario (0 = ocioso). Los eventos usan el `id` de la tarea, no la ranura.

No observado: el LED de ranura de una tarea agregada en marcha.

### 9.9 Apertura del puerto, arranque y tiempos de espera — [observado por el desarrollador del nodo; no cambia ningún formato]

- **Abrir el puerto reinicia el nodo.** Observado desde Python (pyserial, macOS). Con un host en C sobre Linux **no está verificado**. Tras abrir, el nodo emite unos 304 bytes de texto de arranque que no son tramas (incluyen la cadena `rst:0x...` del ROM). El host debe descartar bytes hasta ver la marca `A5 5A` y validar el CRC. El host de prueba espera hasta 0,5 s de silencio antes de enviar la primera trama.
- **Cerrar el puerto sin `Stop` no reinicia el nodo ni detiene su ejecución** (observado por LEDs, un cierre desde Python en macOS). Por tanto, una reconexión del host debe tratarse como un nodo nuevo: volver a calibrar y a desplegar.
- **Ruido residual.** Dos veces, justo después de cargar el firmware, el primer ACK llegó con el encabezado dañado. No se reprodujo y la causa no está explicada. Resincronizar por `A5 5A` más CRC lo tolera.
- **Calibración.** Dura 50 ticks del tick solicitado (10 ms: ~0,5 s; 1 ms: ~50 ms; 200 ms: ~10 s observado). El timeout del host para `CalibrateResp` debe ser mayor que 50 × tick más un margen (el host de prueba usa 50 × tick + 5 s).
- **Una orden a la vez.** Mientras el nodo calibra, otras tramas esperan en el búfer de recepción (lectura del código, no probado). El host no debe enviar otra orden hasta recibir la respuesta.
- **Stop.** Ver la latencia medida en 9.2.

### 9.10 Tipos desconocidos y entradas inválidas — [probado por cable]

- **Tipo sin función.** Cualquier tipo de mensaje que el nodo no atiende (los que el nodo envía al host, `0x02`, `0x05`, `0x06` y `0x09`, y los no definidos) recibe `ACK(tipo, 2)` ("no implementado"). Probado por cable con los 256 valores de tipo y carga vacía: cada uno respondió el ACK esperado.
- **Cargas y límites.** `DeployTasks` y `AddTask` rechazan con `ACK(tipo, 1)` cargas de 512 B sin formato, líneas de más de 95 caracteres y tablas de más de 8 tareas.
- **Fuzz.** Con 400 entradas aleatorias (tramas con CRC válido y carga aleatoria, bits cambiados, ruido, longitud declarada excesiva y tramas truncadas; semilla 4303, `test_invalid_inputs.py`) el nodo no se reinició (verificado con el texto de arranque del ROM), siguió respondiendo, quedó en reposo tras `Stop` y ejecutó igual que el modelo.
- **Tramas a medias (lectura del código, no probado por separado).** Si el host envía una trama corrupta o incompleta, el nodo puede descartar también las tramas siguientes que lleguen antes de que venza el timeout de 100 ms entre bytes. Tras un error de transmisión, esperar al menos 100 ms antes de reenviar.

### 9.11 Pendientes conocidos del nodo

- **Decisiones de formato con el host:** evento BAD_FRAME (hoy las tramas malas solo se cuentan en `TickReport`) y evento de fin de ejecución en `Stop`.
- **Sincronización y desfase entre nodos:** cómo medirlo y documentarlo se acordará con el host (ver 9.1 para la deriva medida).
- **Sin probar:** desconexión física del cable USB y reconexión; rebotes reales del botón menores de 50 ms; motivo 1 de SPORADIC_IGNORED; latencia del ACK de `AddTask`; orden entre el ACK de `AddTask` y `TASK_ADDED`; `AddTask` a 1 ms; `Start` con la tabla vacía; tick de 1 s; corridas de más de 10 minutos; LED de ranura de una tarea agregada en marcha.
- **Sin ocurrencia natural:** `TICK_LOST` y `ticks_perdidos` distinto de 0 solo se observaron con un firmware de prueba que bloquea el calendarizador.

### 9.12 Scripts de prueba del nodo

Están en `host_tests/`. Todos se ejecutan con `python` y la opción `--port` (por defecto `/dev/cu.usbserial-0001`, de macOS; en Linux, por ejemplo, `--port /dev/ttyUSB0`). Requieren `pyserial` y el monitor serie cerrado. `test_start_stop.py` contiene la clase `Node`, una implementación de referencia en Python del lado del host (armado de tramas, CRC-16, lectura y despacho por tipo) que el equipo del host puede consultar.

| Script | Qué verifica | Resultado |
|---|---|---|
| `test_start_stop.py` (`--alg edf` / `rms`) | Calibración, `DeployTasks`, `Start`, `Stop` y estados de ACK | 18 de 18 en EDF y en RMS |
| `test_events.py` | Flujo de `Event` contra un modelo, RMS y EDF | 49 de 49 |
| `test_tickreport.py` | `TickReport`: periodo, contadores y tramas malas | 65 de 65 |
| `test_sporadic.py` (`--button`) | Esporádica con `SporadicRelease` y con el botón real | 43 de 43; 57 de 57 con botón |
| `test_addtask.py` | `AddTask` en reposo y en ejecución, rechazos, persistencia | 103 de 103 |
| `test_stress.py` | Tick de 1 ms, sobrecarga, búfer lleno | 35 de 35 |
| `test_host_gone.py` (`--vanish`) | El nodo sigue ejecutando sin host | 9 de 9; manual por LEDs |
| `test_invalid_inputs.py` | 256 tipos, límites, parser estricto, fuzz | 33 de 33 |
| `test_long_run.py` | Corrida larga, deriva del reloj | 17 de 17 (10 min) |
| `test_tick_lost.py` | `TICK_LOST` (requiere el firmware de prueba de `R2_inyeccion_de_fallos.md`) | 15 de 15 |

Pruebas aisladas en el computador (sin placa, `g++ -std=c++17`): `sched_core_test.cpp` (168 comprobaciones), `tick_report_test.cpp` (96), `task_parser_test.cpp` (38) y `event_queue_test.cpp` (sin fallos). Todas sin fallos.

---

## 10. Límites del nodo

| Límite | Valor | Estado |
|---|---|---|
| Carga útil máxima | 512 B | probado por cable (cargas de 512 B en `test_invalid_inputs.py`) |
| `tick_us` | 1000 a 1 000 000 | probado por cable: rechazo de 5; ejecución a 1 ms, 10 ms y 200 ms. El tick de 1 s no se probó |
| Tareas simultáneas | 8 | probado por cable (`DeployTasks` de 8 tareas con una esporádica aceptado y de 9 rechazado; `AddTask` hasta 8) |
| Línea de tarea | ≤ 95 caracteres | probado por cable (95 aceptada y 96 rechazada, en `DeployTasks` y `AddTask`) |
| Esporádicas por nodo | 1 | probado por cable (`DeployTasks` y `AddTask`) |
| `id` de tarea | ≤ 65535 | probado por cable (`DeployTasks` y `AddTask`) |
| Búfer de eventos | 256 registros | probado por cable (lleno: descarta el nuevo y lo cuenta) |
| Capacidad sostenida de eventos | ~1258 eventos/s | medido por cable (límite de la UART a 115200 baudios) |
| Eventos por trama | 56 | probado por cable |
| `N` del `TickReport` | 0 a 65535 | implementado; probado por cable con 0, 1, 10, 20 y 100 |

---

## 11. Registro de cambios

- **Borrador 10:** cambios de formato: `AddTask` (0x0A) y evento `TASK_ADDED` (nuevos, pendientes de acordar con el host; `AddTask` no usa el estado 3). Sin otros cambios de formato. Resumen de lo que se agregó y probó por cable:
  - `AddTask` y `TASK_ADDED` (`test_addtask.py`, 103 de 103).
  - Estrés a 1 ms y sobrecarga (`test_stress.py`, 35 de 35): búfer de eventos lleno, contadores saturados, capacidad de ~1258 eventos/s, `Stop` con ACK a los 338 ms bajo saturación y reinicio limpio posterior. **Hallazgo corregido:** bajo saturación sostenida de eventos el `TickReport` no salía; ahora sale entre lotes de eventos (sin cambio de formato).
  - Host desconectado (`test_host_gone.py`, automático 9 de 9; manual por LEDs): el nodo sigue ejecutando sin `Stop`.
  - Entradas inválidas (`test_invalid_inputs.py`, 33 de 33): barrido de los 256 tipos, límites exactos de línea y tabla, 400 entradas de fuzz sin reinicio. **Parser más estricto:** rechaza comas dobles, iniciales y finales, y un `DeployTasks` con byte NUL (antes se aceptaban; no cambia el formato, solo es más estricto). Prueba aislada del parser: 38 de 38.
  - Corrida larga de 10 minutos (`test_long_run.py`, 17 de 17): 60 011 ticks y 78 017 eventos idénticos al modelo, 600 reportes sin huecos, sin reinicios, ocupación máxima 79 % del tick, deriva del reloj −70 ppm.
  - `TICK_LOST` y `ticks_perdidos` (`test_tick_lost.py`, 15 de 15, con un firmware de prueba que bloquea el calendarizador; el firmware entregado no contiene esa inyección): dos `TICK_LOST` exactos, flujo idéntico al modelo con saltos de tiempo y reportes sin ráfaga tras el salto.
  - Secciones nuevas 9.9 (apertura del puerto y tiempos), 9.10 (entradas inválidas), 9.11 (pendientes) y 9.12 (scripts de prueba). Tabla de límites (sección 10) actualizada con la evidencia.
  - Limpieza interna del nodo (se retiró código de pruebas no usado), sin cambios de comportamiento.
  - Regresión: `test_start_stop.py` 18 de 18 (EDF y RMS), `test_events.py` 49 de 49 y `test_tickreport.py` 65 de 65 tras el arreglo del reporte, `test_start_stop.py` 18 de 18 tras el parser, `test_sporadic.py` 43 de 43 repetido tras ambos arreglos. La prueba con el botón real (57 de 57) se corrió con el firmware de `AddTask` y no se ha repetido.
- Borrador 9: sin cambios de formato. Intervalo mínimo de la esporádica probado con el botón real (15 pulsaciones = 9 liberaciones + 6 ignoradas).
- Borrador 8: sin cambios de formato. Tarea esporádica probada por cable (`test_sporadic.py`: 43 de 43 con `SporadicRelease`; 57 de 57 con el botón real, 6 pulsaciones lentas).
- Borrador 7: tarea esporádica implementada (botón GPIO 27 con antirrebote de 50 ms, intervalo mínimo, `SporadicRelease`, evento SPORADIC_IGNORED con motivo en `arg`, contador `esporadicas_ignoradas`). Cambio de formato: el campo `arg` de SPORADIC_IGNORED pasa de "siempre 0" a un código de motivo. Sin probar por cable.
- Borrador 6: sin cambios de formato. `TickReport` probado por cable (`test_tickreport.py`).
- Borrador 5: `TickReport` implementado (22 bytes: se agregó `max_latencia_us` al final del formato propuesto); `Start` reinicia el contador de tramas malas. Sin probar por cable.
- Borrador 4: sin cambios de formato. `Event` (RELEASE, SWITCH, COMPLETE, MISS), `Stop` con entrega previa de eventos y envío atómico de tramas probados por cable (`test_events.py`, 49 de 49; regresión de `test_start_stop.py`, 18 de 18).
- Borrador 3: `Event` implementado (RELEASE, SWITCH, COMPLETE, MISS, TICK_LOST), búfer circular de 256 registros con transmisión en segundo plano; `Stop` entrega los eventos pendientes antes de su ACK; los `Event` pueden llegar antes que el ACK de `Start`; las tramas salientes (ACK, `CalibrateResp`, `Event`) se envían de forma atómica. Sin cambios en los mensajes ya existentes. Sin probar por cable.
- Borrador 2: `Start`/`Stop`, estado 3, rechazo de `id` > 65535 y de segunda esporádica, y calibración con el nodo en ejecución probados por cable (script de prueba, 18 de 18 comprobaciones de ACK en EDF y en RMS). Sin cambios de formato.
- Borrador 1: `Start` (carga `[algoritmo][N]`) y `Stop` implementados; ACK estado 3 en uso; `CalibrateReq`/`DeployTasks` responden estado 3 con el nodo en ejecución; `DeployTasks` rechaza `id` > 65535 y una segunda esporádica; la calibración corre en el núcleo del calendarizador. Sin probar por cable.
- Borrador 0: documento inicial. Solo la sección 9 es nueva respecto al código existente; el resto documenta lo ya verificado.