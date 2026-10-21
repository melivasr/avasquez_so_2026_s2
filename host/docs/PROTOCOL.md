# Protocolo e integración

## Auditoría del repositorio

ESP32 usa PlatformIO, ESP-IDF, placa esp32dev y UART0 a 115200 (TX1/RX3).
`app_main` llama `serial_receiver_run`. Hay calibración GPTimer de 50 ticks con
margen del 80 %, recepción parcial con timeout, CRC y parser de tareas CSV.
DEPLOY valida hasta ocho tareas y reemplaza su tabla completa; todavía no hay
calendarizador conectado a esa tabla. START/STOP/EVENT/TICK/SPORADIC/CONFIGURE/
ADD_TASK no están implementados y reciben ACK estado 2 cuando llegan como orden.
`timer_run_test` es un ensayo separado, no el calendarizador requerido.

Pico usa PlatformIO raspberrypi, framework arduino, board pico (también hay un
env nanorp2040connect). Es un RP2040, sin Linux. Existen encode/decode, CRC,
FrameReceiver y EventQueue de 64 entradas con los ocho códigos de evento.
El main todavía parpadea un LED con delay; no conecta esos módulos al USB ni
implementa calibración, tareas, temporizador local o botón. No se cambió su BSP.

Los dos codecs ya coinciden en marca, tipos 1..9, límites y CRC. Las pruebas
`interop_esp`/`interop_pico` compilan sus implementaciones originales y verifican
compatibilidad binaria con C del host. No prueban el SDK ni firmware completo.

## Trama existente, preservada

```text
A5 5A | tipo:u8 | longitud:u16 LE | payload[longitud] | CRC:u16 LE
```

Carga máxima 512; trama máxima 519 bytes. CRC-16/CCITT-FALSE: polinomio 0x1021,
inicial 0xFFFF, no reflejado, xorout 0. Cubre tipo, ambos bytes de longitud y
payload; excluye marca y CRC. Vector «123456789» = 0x29B1.
Todos los enteros multibyte viajan little-endian. Nunca se serializa un struct
C/C++ por memcpy: puede contener padding. CSV viaja ASCII, sin NUL.

## Cargas y contrato v1

Las filas marcadas «existente» reflejan firmware efectivo. Las demás fijan el
contrato **pendiente de implementar en los nodos** para interoperar con el host.
Se preserva la codificación existente de DEPLOY y calibración. Las extensiones
CONFIGURE/ADD_TASK ocupan tipos nuevos porque DEPLOY ya significa reemplazo.

| Tipo | Dirección | Payload exacto | Estado |
|---|---|---|---|
| 0x01 CAL_REQ | host→nodo | tick_us:u32 | existente ESP32 |
| 0x02 CAL_RESP | nodo→host | s_n:u32 positivo, margen ya aplicado | existente ESP32 |
| 0x03 DEPLOY | host→nodo | 1..8 líneas `id,tipo,W,T,D,criticidad\n` | existente ESP32 |
| 0x04 START | host→nodo | vacío | pendiente |
| 0x05 EVENT | nodo→host | tick:u32, tipo_evento:u8, id:u32, valor:u32 = 13 bytes | códigos/estructura existen en Pico; transmisión pendiente |
| 0x06 TICK | nodo→host | tick:u32 = 4 bytes | pendiente |
| 0x07 SPORADIC | host→nodo | id:u32 = 4 bytes | pendiente |
| 0x08 STOP | host→nodo | vacío | pendiente |
| 0x09 ACK | nodo→host | tipo_original:u8, estado:u8 = 2 bytes | existente ESP32 |
| 0x0A CONFIGURE | host→nodo | versión:u8=1, algoritmo:u8 (EDF=0,RMS=1), report_ticks:u32 = 6 bytes | extensión pendiente |
| 0x0B ADD_TASK | host→nodo | exactamente una línea CSV como DEPLOY | extensión pendiente |

ACK estado 0=éxito, 1=contenido/estado inválido, 2=orden no implementada.
El host considera otros estados no cero como rechazo. CAL_RESP es la confirmación
de calibración; ACK(1,0) solo no reemplaza CAL_RESP. Para las demás órdenes se
requiere ACK coincidente. Completar write no equivale a desplegar una tarea.

CONFIGURE debe aceptarse únicamente antes de START, con capacidad local ≥8 y
sin trabajos activos de una sesión anterior. No reinicia tareas de una sesión
desconectada. Para iniciar otra sesión después de desconectar host, hacer STOP
voluntario o reset físico del nodo primero. CONFIRMAR versión 1 significa
soportar las reglas de este contrato; un nodo legacy se excluye con motivo claro.
No hay negociación que pretenda que el firmware incompleto ya las satisface.

DEPLOY reemplaza la tabla solo estando detenido. Validación y commit atómicos:
si una línea falla, no se modifica ninguna tarea. Como máximo una S; IDs únicos.
Una tabla vacía inicial no se envía: CONFIGURE requiere estado detenido/limpio.
El host envía W, no C; el nodo calcula C con la misma s_n informada.

START establece cero exactamente una vez y ACK se envía después de establecerlo.
A partir de entonces un nodo sin tareas también emite TICK cada report_ticks.
ADD_TASK agrega atómicamente sin reemplazar tabla, reiniciar cero o tocar estados
de otros trabajos. La primera liberación periódica será en el siguiente tick
local. Una S queda esperando evento. El mismo ID con definición idéntica debe
ser idempotente (sin nueva liberación); con definición distinta se rechaza.
El host usa IDs únicos por sesión, pero no depende de retransmitir ADD_TASK.
El parser ESP32 actual usa criticidad uint32; debe aceptar int32 con signo según
el contrato del host, conservando el resto de campos e interfaz UT.

SPORADIC representa una solicitud de liberación, no un turno de CPU. Botón físico
y solicitud software pasan por la misma comprobación local de intervalo mínimo.
ACK confirma recepción/aplicación de esa comprobación; el EVENT correspondiente
indica Release o SporadicIgnored. El host no hace debounce ni suple interrupciones.
STOP es idempotente, aborta/detiene trabajos locales y confirma tras hacerlo;
puede transmitir eventos finales. Desconexión USB/silencio del host no es STOP.

## Eventos

Se usan los códigos ya definidos en `node2/include/event_queue.h`:

| Código | Nombre | tick/valor |
|---|---|---|
| 1 | Release | tick de liberación; valor = plazo absoluto (ticks) |
| 2 | Run | tick inicial; valor = duración positiva en ticks ejecutados contiguos |
| 3 | Complete | tick límite de finalización, valor = 0 |
| 4 | DeadlineMiss | tick del plazo incumplido, valor = plazo absoluto; trabajo abortado |
| 5 | Idle | tick inicial de idle, id=UINT32_MAX; valor=duración si se conoce |
| 6 | SporadicIgnored | tick del intento ignorado; id de S; valor = número de intentos ignorados en este evento |
| 7 | FrameError | tick local; id=UINT32_MAX; valor = errores desde último evento |
| 8 | EventsLost | tick local; id=UINT32_MAX; valor = registros perdidos desde último aviso |

RUN describe `[tick,tick+valor)`, ya observado; nunca una predicción de lo que
correrá. Puede enviarse por tick o agrupado, pero no solaparse ni duplicarse.
Eventos ordenados por tick dentro de cada nodo; para un trabajo: Release, Run,
Complete/Miss. La marca de Complete representa el límite posterior a su último
tick de ejecución. Miss se emite al abortar el trabajo. Reporte toma solo RUN
para construir el Gantt, rechaza intervalos no positivos/solapados y marca
la traza incompleta. Los periodos/plazos se conservan en tasks.csv y los plazos
absolutos en eventos Release/Miss.

La estructura existente no incluye secuencia de telemetría ni ID de trabajo.
El contrato admite un trabajo activo por ID; si el firmware permite trabajos
solapados (por ejemplo D>T), necesita extender eventos con ID de trabajo para
correlación completa. El host acepta D>T y muestra eventos, pero deja sin muestras
las correlaciones ambiguas; no inventa latencias. Los contadores de la Pico
son códigos preparados, aún no instrumentados. No se detectan todas las pérdidas
si firmware no envía EventsLost; se documenta esta limitación.

Tick es uint32 desde START: esta versión no extiende rollover. Mantener las
sesiones por debajo de 2^32 ticks (≈497 días con tick de 10 ms); el firmware debe
usar aritmética adecuada para plazos y notificar cualquier reinicio inesperado.

## Recuperación y transacciones

El host procesa bytes parciales, varias tramas juntas y ruido previo a la marca.
Ante CRC/longitud inválida descarta bytes y busca nueva marca; tras 200 ms sin
completar una trama descarta el fragmento. Cuenta/informa errores. Los huecos
perdidos quedan como telemetría incompleta. Una trama con CRC correcto y contenido
no previsto se registra como inesperada y no confirma despliegues.

Un solo intercambio pendiente por controlador. Poll bloqueante espera puerto y
eventfd; write maneja parcialidad y tiene un plazo total. La respuesta tiene
`timeout_ms` adicional desde que finalizó el envío. No se retransmite ninguna
orden en esta versión: **cero reintentos automáticos**, un límite deliberado.
El ACK actual solo identifica tipo y no permite distinguir un ACK tardío de uno
de una repetición. Tras timeout se declara resultado incierto, se conserva la
reserva hasta procesar caída y se excluye ese nodo de nuevas admisiones. No se
reutiliza su canal para otro despliegue. Se puede solicitar STOP al cerrar.
Para habilitar reintentos de efectos se necesitarían IDs de transacción y caché
de respuestas en ambos firmwares, o consulta fiable del estado instalado.

Rechazo explícito libera la reserva de ADD_TASK; comunicación perdida no se
registra como rechazo definitivo ni como confirmación. La reubicación posterior
es una decisión por caída y se distingue en eventos; no resuelve la posibilidad
de que el nodo aislado siga ejecutando (ver README, particiones y fencing).

## Lista concreta para cada compañero

ESP32: conservar UART/pines/ESP-IDF, codecs, UT y calibración; conectar la tabla
validada al calendarizador local; implementar CONFIGURE, START, ADD_TASK, TICK,
STOP y SPORADIC según arriba; instrumentar ocho eventos con transmisión de fondo,
contador de pérdidas, botón/debounce/intervalo mínimo y LEDs. No reemplazar DEPLOY
por append: el host distingue ambos tipos. Actualizar criticidad a int32.

Pico: conservar PlatformIO/board/pines escogidos; conectar codecs y parser USB al
main; incorporar calibración por tick solicitado con margen, parser CSV y tabla
de al menos ocho tareas; implementar el mismo contrato, scheduler RMS/EDF local,
eventos, heartbeat, botón y LEDs. La cola existente necesita sincronización
correcta entre ISR/productor y consumidor según su uso; no asumir que su clase
actual es segura para acceso concurrente. No se modificó esa implementación.

UT se mantuvo en sus ubicaciones actuales para no romper compilaciones que no
pueden verificarse aquí. `host/ut.c` y ESP32 son idénticos; la copia Pico ya tenía
un `#include "ut.h"` adicional. No se borró ni normalizó ninguna copia.
`UT_SHA256SUMS` conserva los hashes iniciales y verifica que no cambiaron.
