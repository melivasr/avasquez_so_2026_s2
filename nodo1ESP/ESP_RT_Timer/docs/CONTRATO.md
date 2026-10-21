# README_NODO — Nodo ESP32 (contrato serial para el host)

Documento para quienes desarrollan el host. El contrato entre host y nodo es **solo** el protocolo serial descrito aquí; no hace falta conocer el interior del nodo.

## Leyenda de estados

| Etiqueta | Significado |
|---|---|
| **[probado por cable]** | Verificado con un host de prueba en Python contra un ESP32 real. |
| **[implementado, sin probar por cable]** | Existe en el código del nodo; sin evidencia por cable. |
| **[propuesto — pendiente de acordar con el host]** | Solo diseño. No existe en el nodo. No programar contra esto hasta acordarlo. |

Versión del documento: borrador 8 (tarea esporádica probada por cable con el host y con el botón real; faltan el intervalo mínimo y las pulsaciones rápidas con el botón).

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

> Cada subsección indica su estado. Lo que no está marcado como implementado es **[propuesto — pendiente de acordar con el host]** y no existe en el nodo. Todo formato nuevo está pendiente de acordar con el host aunque ya esté implementado.

### 9.1 Start (0x04), host → nodo — [probado por cable a nivel de ACK]

Probado por cable: `Start` válido → estado 0 (con `N = 0`); carga de 2 bytes → 1; algoritmo 2 → 1; repetido → 3; sin calibrar → 3; `Start` tras `Stop` → 0. **No probado por cable:** `N` distinto de 0 y tabla vacía. El flujo de `Event` sí se probó (9.5). La ejecución de las tareas se observó por LEDs en EDF y RMS, congruente con lo esperado (observación del desarrollador del nodo).

Carga de 3 bytes: `[algoritmo: 1 B][N: 2 B LE]`

- `algoritmo`: 0 = RMS, 1 = EDF.
- `N`: periodo del `TickReport` en ticks; 0 desactiva los reportes. El nodo acepta cualquier valor (0 a 65535). Ver 9.6.
- Prerrequisito: haber calibrado. El tick de ejecución es el `tick_us` de la última calibración exitosa (`Start` no lo lleva). La tabla puede estar vacía (un nodo sin tareas asignadas igual puede iniciarse).
- Efecto: el **tick cero** es el instante en que el nodo termina de procesar la trama; luego envía `ACK(0x04, 0)`. Todos los contadores se reinician.
- `ACK(0x04, 3)` si ya está en ejecución o no hubo calibración. `ACK(0x04, 1)` si la longitud de la carga ≠ 3, el algoritmo no es 0 ni 1, o el nodo no pudo iniciar la ejecución (tabla inválida o fallo del temporizador).
- El ACK se envía justo después de iniciar el temporizador; el tick cero se procesa a continuación. Los `Event` del tick cero pueden llegar **antes** que el ACK de `Start`: el host debe despachar las tramas por tipo, no por orden de llegada.
- El LED de plazo perdido se apaga en cada `Start`.
- Desfase entre nodos: el host inicia los nodos en secuencia, así que habrá un desfase de pocos ms por nodo. Cómo medirlo está por acordar.

### 9.2 Stop (0x08), host → nodo — [probado por cable a nivel de ACK]

Probado por cable: `Stop` en ejecución → 0; repetido o sin ejecución → 3; con carga → 1. No se midió la latencia del ACK.

Carga vacía (otra longitud → `ACK(0x08, 1)`). El nodo termina el tramo en curso, detiene el temporizador y envía `ACK(0x08, 0)`; el ACK puede tardar hasta un tick. `ACK(0x08, 3)` si no estaba en ejecución. Vuelve a estado inactivo (la tabla se conserva; un nuevo `DeployTasks` la reemplaza y se puede volver a enviar `Start`). Antes de enviar el ACK, el nodo entrega todos los `Event` pendientes: después del ACK de `Stop` no llegan más `Event`. **[probado por cable]** El nodo no emite un evento de fin: el Gantt termina en el último tick conocido (punto abierto, ver 9.5).

Si el host se detiene o desconecta **sin enviar Stop**, el nodo sigue ejecutando sus tareas.

### 9.3 DeployTasks mientras hay ejecución — [probado por cable]

Se rechaza con `ACK(0x03, 3)`. Para agregar tareas en marcha se usa `AddTask`.

### 9.4 AddTask (0x0A), host → nodo (modo manual)

Carga: **una** línea de texto con el mismo formato que en `DeployTasks`, sin `\n`. Válido en estado inactivo y en ejecución. El host ya hizo la admisión; el nodo solo valida formato, ID único, tabla no llena y una sola esporádica.

- En ejecución, la tarea se activa en el siguiente límite de tick; el nodo emite un evento `TASK_ADDED` con el tick de primera liberación.
- Respuestas: `ACK(0x0A, 0)` aceptada, `1` rechazada, `3` sin calibración.

### 9.5 Event (0x05), nodo → host — [RELEASE, SWITCH, COMPLETE y MISS probados por cable]

Probado por cable (conjuntos de tareas A y B, RMS y EDF, tick de 10 ms, 300 ticks comparados, 49 de 49 comprobaciones): RELEASE, SWITCH, COMPLETE y MISS coinciden evento por evento con un modelo del calendarizador; marcas de tiempo no decrecientes; tramas bien formadas; sin TICK_LOST; ningún `Event` después del ACK de `Stop`. **No probado:** búfer lleno (descartes), ticks de 1 ms, corridas largas (cada corrida duró ~4 s) y el propio TICK_LOST (implementado, nunca provocado). SPORADIC_IGNORED con motivo 0 (vía `SporadicRelease`) está probado por cable; el motivo 1 (sin tarea esporádica, solo ocurre con el botón) no se ha probado. **No implementados:** TASK_ADDED y BAD_FRAME (se agregan con sus funciones).

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
| 0x07 | TICK_LOST | 0 | ticks perdidos |
| 0x08 | BAD_FRAME (máx. una vez por tick) | 0 | total acumulado |

Convención: un SWITCH en el tick *k* significa que el tramo [*k*, *k*+1) pertenece a esa tarea hasta el siguiente SWITCH. COMPLETE se estampa con el tick en que termina el último tramo del trabajo. Con esto el host reconstruye el Gantt.

Límite que esto introduce: **`id` ≤ 65535** (el nodo rechaza IDs mayores).

### 9.6 TickReport (0x06), nodo → host — [probado por cable]

Probado por cable (tick de 10 ms): periodo `N` = 20, 10, 1 y 0 (sin huecos entre reportes; con `N` = 1, 205 reportes consecutivos en ~2 s); contadores no decrecientes; `plazos_perdidos` igual a los MISS recibidos hasta cada tick; `tramas_malas` exacto (5 tramas con CRC malo + 1 truncada = 6) sin alterar la ejecución; ningún reporte después del ACK de `Stop`. **No probado:** corridas largas, ticks de 1 ms, `ticks_perdidos` y `eventos_descartados` distintos de 0 (siempre fueron 0), `esporadicas_ignoradas` distinto de 0 en esas pruebas (se probó aparte, ver 9.7).

Medición del margen de calibración (una sola corrida de ~3 s, conjunto B con EDF, tick de 10 ms, `ut_por_tick` = 70): `max_ocupado_us` = 7907 µs (79 % del tick) y `max_latencia_us` = 20 µs. Es una medición puntual, no una cota garantizada.

> **Cambio respecto a la propuesta anterior (18 bytes):** se agregó `max_latencia_us` al final. Un parser que lea solo los primeros 18 bytes sigue funcionando; se recomienda validar por longitud (22 bytes).

Se envía cada `N` ticks (el `N` de `Start`; 0 = no se envía). Carga de 22 bytes, little endian:

```
[tick: 4 B][plazos_perdidos: 2 B][esporadicas_ignoradas: 2 B][ticks_perdidos: 2 B]
[eventos_descartados: 2 B][tramas_malas: 2 B][max_ocupado_us: 4 B][max_latencia_us: 4 B]
```

- **`tick`:** último tick procesado. El primer reporte sale en el tick `N`, luego en `2N`, `3N`... Si se pierden ticks, el reporte siguiente sale en el siguiente múltiplo de `N` (no hay ráfagas).
- **Contadores:** acumulados desde `Start`. Los de 16 bits saturan en 65535.
- **`plazos_perdidos`:** coincide con la cantidad de eventos MISS hasta ese `tick`.
- **`esporadicas_ignoradas`:** pulsaciones de la esporádica ignoradas (intervalo mínimo no cumplido o sin tarea esporádica); coincide con los eventos SPORADIC_IGNORED hasta ese `tick`, salvo que por tick solo se emiten los primeros 8 eventos de ignoradas (el contador sí cuenta todas). **[probado por cable con 1 ignorada, que coincidió con el evento]**
- **`ticks_perdidos`:** ticks que el nodo no alcanzó a procesar.
- **`eventos_descartados`:** eventos perdidos por búfer lleno (9.5).
- **`tramas_malas`:** tramas recibidas del host y descartadas por CRC o longitud inválidos, por exceder el tamaño máximo o por quedar incompletas (timeout entre bytes). El ruido antes de una marca de inicio no cuenta.
- **`max_ocupado_us`:** mayor tiempo de proceso dentro de un tick (incluye ejecutar las UT del tramo). Dividido entre la duración del tick indica la ocupación; la calibración deja un margen del 80 %, por lo que se espera un valor cercano o por debajo de ese porcentaje.
- **`max_latencia_us`:** mayor retraso entre la alarma del temporizador y el inicio del proceso del tick.

Reglas de uso para el host:

- `Event` y `TickReport` pueden intercalarse: los eventos de un tick posterior pueden llegar antes que el reporte de un tick anterior. Usar el campo `tick`, no el orden de llegada.
- Se transmite en segundo plano con prioridad baja. Si un reporte no alcanza a salir antes de que se prepare el siguiente, se reemplaza (solo importa el más reciente); con `N` = 1 no se observaron reemplazos en la prueba (ver arriba).
- Tras el ACK de `Stop` no llega ningún `TickReport` más.
- Para declarar un nodo caído, el host puede contar reportes consecutivos ausentes (cada reporte indica que el calendarizador sigue vivo).

### 9.7 Tarea esporádica, botón y SporadicRelease (0x07) — [probado por cable]

Probado por cable (tick de 10 ms, EDF y RMS, tarea esporádica con C = 2, T = D = 50 ticks junto a una periódica): la esporádica no se libera sola; tres `SporadicRelease` producen 2 liberaciones y 1 ignorada (motivo 0), separadas al menos `T`; números de trabajo consecutivos; cada liberación tiene su COMPLETE; latencia de respuesta de 2 a 3 ticks (medida liberación → fin, con la periódica de mayor prioridad); sin plazos perdidos; `ACK(0x07, 3)` sin ejecución, `ACK(0x07, 1)` sin esporádica o con carga no vacía. Con el botón real (GPIO 27): 6 pulsaciones lentas produjeron exactamente 6 liberaciones, ninguna ignorada y ningún rebote contado (incluido el rebote al soltar); latencias de 2 a 3 ticks. **No probado por cable:** pulsaciones rápidas con el botón (intervalo mínimo con el botón real), pulsaciones separadas menos de 50 ms, el motivo 1 de SPORADIC_IGNORED, dos pulsaciones en un mismo tick y `D` > `T` con la esporádica (solo en pruebas aisladas).

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

## 10. Límites del nodo

| Límite | Valor | Estado |
|---|---|---|
| Carga útil máxima | 512 B | probado por cable |
| `tick_us` | 1000 a 1 000 000 | probado por cable (rechazo de 5) |
| Tareas simultáneas | 8 | implementado, sin probar por cable |
| Línea de tarea | ≤ 95 caracteres | probado solo en memoria |
| Esporádicas por nodo | 1 | implementado en `DeployTasks`, sin probar por cable |
| `id` de tarea | ≤ 65535 | implementado en `DeployTasks`, sin probar por cable |

## 11. Registro de cambios

- Borrador 0: documento inicial. Solo la sección 9 es nueva respecto al código existente; el resto documenta lo ya verificado.
- Borrador 8: sin cambios de formato. Tarea esporádica probada por cable (`test_sporadic.py`: 43 de 43 con `SporadicRelease`; 57 de 57 con el botón real, 6 pulsaciones lentas).
- Borrador 7: tarea esporádica implementada (botón GPIO 27 con antirrebote de 50 ms, intervalo mínimo, `SporadicRelease`, evento SPORADIC_IGNORED con motivo en `arg`, contador `esporadicas_ignoradas`). Cambio de formato: el campo `arg` de SPORADIC_IGNORED pasa de "siempre 0" a un código de motivo. Sin probar por cable.
- Borrador 6: sin cambios de formato. `TickReport` probado por cable (`test_tickreport.py`).
- Borrador 5: `TickReport` implementado (22 bytes: se agregó `max_latencia_us` al final del formato propuesto); `Start` reinicia el contador de tramas malas. Sin probar por cable.
- Borrador 4: sin cambios de formato. `Event` (RELEASE, SWITCH, COMPLETE, MISS), `Stop` con entrega previa de eventos y envío atómico de tramas probados por cable (`test_events.py`, 49 de 49; regresión de `test_start_stop.py`, 18 de 18).
- Borrador 3: `Event` implementado (RELEASE, SWITCH, COMPLETE, MISS, TICK_LOST), búfer circular de 256 registros con transmisión en segundo plano; `Stop` entrega los eventos pendientes antes de su ACK; los `Event` pueden llegar antes que el ACK de `Start`; las tramas salientes (ACK, `CalibrateResp`, `Event`) se envían de forma atómica. Sin cambios en los mensajes ya existentes. Sin probar por cable.
- Borrador 2: `Start`/`Stop`, estado 3, rechazo de `id` > 65535 y de segunda esporádica, y calibración con el nodo en ejecución probados por cable (script de prueba, 18 de 18 comprobaciones de ACK en EDF y en RMS). Sin cambios de formato.
- Borrador 1: `Start` (carga `[algoritmo][N]`) y `Stop` implementados; ACK estado 3 en uso; `CalibrateReq`/`DeployTasks` responden estado 3 con el nodo en ejecución; `DeployTasks` rechaza `id` > 65535 y una segunda esporádica; la calibración corre en el núcleo del calendarizador. Sin probar por cable.