# README_NODO — Nodo ESP32 (contrato serial para el host)

Documento para quienes desarrollan el host. El contrato entre host y nodo es **solo** el protocolo serial descrito aquí; no hace falta conocer el interior del nodo.

## Leyenda de estados

| Etiqueta | Significado |
|---|---|
| **[probado por cable]** | Verificado con un host de prueba en Python contra un ESP32 real. |
| **[implementado, sin probar por cable]** | Existe en el código del nodo; sin evidencia por cable. |
| **[propuesto — pendiente de acordar con el host]** | Solo diseño. No existe en el nodo. No programar contra esto hasta acordarlo. |

Versión del documento: borrador 4 (`Start`, `Stop` y `Event` probados por cable; la ejecución se contrastó contra un modelo independiente).

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
| TickReport | 0x06 | nodo → host | propuesto (sección 9) |
| SporadicRelease | 0x07 | host → nodo | propuesto (sección 9) |
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

5. `Start` → `ACK(0x04, 0)`; el nodo ejecuta las tareas y empieza a enviar `Event` (probado por cable). `TickReport` todavía no se envía. Los primeros `Event` pueden llegar antes que el ACK de `Start`.
7. `Stop` → `ACK(0x08, 0)`.

Propuesto, no existe en el nodo:

6. (Modo manual) `AddTask`; (opcional) `SporadicRelease`. `TickReport` se emitirá entre los pasos 5 y 7.

## 9. Propuestas pendientes de acordar con el host

> Cada subsección indica su estado. Lo que no está marcado como implementado es **[propuesto — pendiente de acordar con el host]** y no existe en el nodo. Todo formato nuevo está pendiente de acordar con el host aunque ya esté implementado.

### 9.1 Start (0x04), host → nodo — [probado por cable a nivel de ACK]

Probado por cable: `Start` válido → estado 0 (con `N = 0`); carga de 2 bytes → 1; algoritmo 2 → 1; repetido → 3; sin calibrar → 3; `Start` tras `Stop` → 0. **No probado por cable:** `N` distinto de 0 y tabla vacía. El flujo de `Event` sí se probó (9.5). La ejecución de las tareas se observó por LEDs en EDF y RMS, congruente con lo esperado (observación del desarrollador del nodo).

Carga de 3 bytes: `[algoritmo: 1 B][N: 2 B LE]`

- `algoritmo`: 0 = RMS, 1 = EDF.
- `N`: periodo del `TickReport` en ticks; 0 desactiva los reportes. Hoy el nodo acepta cualquier valor (0 a 65535) y lo guarda, pero **todavía no envía `TickReport`**.
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

Probado por cable (conjuntos de tareas A y B, RMS y EDF, tick de 10 ms, 300 ticks comparados, 49 de 49 comprobaciones): RELEASE, SWITCH, COMPLETE y MISS coinciden evento por evento con un modelo del calendarizador; marcas de tiempo no decrecientes; tramas bien formadas; sin TICK_LOST; ningún `Event` después del ACK de `Stop`. **No probado:** búfer lleno (descartes), ticks de 1 ms, corridas largas (cada corrida duró ~4 s) y el propio TICK_LOST (implementado, nunca provocado). **No implementados:** SPORADIC_IGNORED, TASK_ADDED y BAD_FRAME (se agregan con sus funciones).

Comportamiento del búfer: 256 registros en RAM, transmitidos en segundo plano por una tarea aparte; el calendarizador solo encola. Si el búfer se llena se **descarta el evento nuevo** y se cuenta; el host no recibe aviso hasta que exista `TickReport` (`eventos_descartados`). El `Start` vacía el búfer. Orden: las marcas `tick` son no decrecientes; COMPLETE se estampa con el tick de fin del tramo. Punto abierto para acordar con el host: al hacer `Stop` no se emite un evento de fin de ejecución; si el host lo necesita para cerrar el Gantt, se propone agregarlo.

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
| 0x05 | SPORADIC_IGNORED (intervalo mínimo) | tarea esporádica (0 si no hay) | 0 |
| 0x06 | TASK_ADDED | tarea | ranura asignada |
| 0x07 | TICK_LOST | 0 | ticks perdidos |
| 0x08 | BAD_FRAME (máx. una vez por tick) | 0 | total acumulado |

Convención: un SWITCH en el tick *k* significa que el tramo [*k*, *k*+1) pertenece a esa tarea hasta el siguiente SWITCH. COMPLETE se estampa con el tick en que termina el último tramo del trabajo. Con esto el host reconstruye el Gantt.

Límite que esto introduce: **`id` ≤ 65535** (el nodo rechaza IDs mayores).

### 9.6 TickReport (0x06), nodo → host

Se envía cada `N` ticks. Carga de 18 bytes, contadores acumulados desde `Start`:

```
[tick: 4 B LE][plazos_perdidos: 2 B][esporadicas_ignoradas: 2 B]
[ticks_perdidos: 2 B][eventos_descartados: 2 B][tramas_malas: 2 B][max_ocupado_us: 4 B LE]
```

`max_ocupado_us` es el mayor tiempo de CPU usado dentro de un tick (sirve para validar el margen de calibración).

### 9.7 SporadicRelease (0x07), host → nodo

Carga vacía. Equivale a pulsar el botón del nodo (pasa por la misma verificación de intervalo mínimo). `ACK(0x07, 0)` registrado; `ACK(0x07, 1)` el nodo no tiene tarea esporádica; `ACK(0x07, 3)` no está en ejecución.

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
- Borrador 4: sin cambios de formato. `Event` (RELEASE, SWITCH, COMPLETE, MISS), `Stop` con entrega previa de eventos y envío atómico de tramas probados por cable (`test_events.py`, 49 de 49; regresión de `test_start_stop.py`, 18 de 18).
- Borrador 3: `Event` implementado (RELEASE, SWITCH, COMPLETE, MISS, TICK_LOST), búfer circular de 256 registros con transmisión en segundo plano; `Stop` entrega los eventos pendientes antes de su ACK; los `Event` pueden llegar antes que el ACK de `Start`; las tramas salientes (ACK, `CalibrateResp`, `Event`) se envían de forma atómica. Sin cambios en los mensajes ya existentes. Sin probar por cable.
- Borrador 2: `Start`/`Stop`, estado 3, rechazo de `id` > 65535 y de segunda esporádica, y calibración con el nodo en ejecución probados por cable (script de prueba, 18 de 18 comprobaciones de ACK en EDF y en RMS). Sin cambios de formato.
- Borrador 1: `Start` (carga `[algoritmo][N]`) y `Stop` implementados; ACK estado 3 en uso; `CalibrateReq`/`DeployTasks` responden estado 3 con el nodo en ejecución; `DeployTasks` rechaza `id` > 65535 y una segunda esporádica; la calibración corre en el núcleo del calendarizador. Sin probar por cable.