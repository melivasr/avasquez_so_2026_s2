# Protocolo serial compartido

Este contrato corresponde al código del host, ESP32 y Pico. Los códigos y
los tamaños se definen en [`common/rt_protocol.h`](../../common/rt_protocol.h).
Los tres componentes deben compilarse desde el mismo checkout del repositorio.
La documentación histórica del ESP32 conserva resultados de pruebas anteriores;
los cambios descritos aquí requieren cargar el firmware actualizado.

## Trama

```text
A5 5A | tipo:u8 | longitud:u16 LE | payload[longitud] | CRC:u16 LE
```

Carga máxima 512 bytes; trama máxima 519 bytes. CRC-16/CCITT-FALSE, polinomio
0x1021, inicial 0xFFFF, sin reflexión ni XOR final. Cubre tipo, longitud y carga;
excluye marca y CRC. Vector `123456789`: 0x29B1. Los enteros multibyte viajan
little-endian y se codifican campo por campo. USB/UART a 115200, 8N1.

## Mensajes

| Tipo | Mensaje | Carga |
|---|---|---|
| 0x01 | CAL_REQ, host → nodo | tick_us:u32, rango 1000..1000000 |
| 0x02 | CAL_RESP, nodo → host | s_n:u32 positivo, con margen ya aplicado |
| 0x03 | DEPLOY, host → nodo | 0..8 líneas CSV; carga vacía borra toda la tabla |
| 0x04 | START, host → nodo | algoritmo:u8 (RMS=0, EDF=1), report_ticks:u16 |
| 0x05 | EVENT, nodo → host | cantidad:u8, seguida de 1..56 registros de 9 bytes |
| 0x06 | TICK_REPORT, nodo → host | 22 bytes, formato abajo |
| 0x07 | SPORADIC, host → nodo | vacía; solicita la única esporádica del nodo |
| 0x08 | STOP, host → nodo | vacía |
| 0x09 | ACK, nodo → host | tipo_original:u8, estado:u8 |
| 0x0A | ADD_TASK, host → nodo | una línea CSV, con LF final opcional |

No existe CONFIGURE. Algoritmo y período de reporte se envían en START.
El enum interno del host tiene otro orden: `start_frame()` realiza la conversión
explícita al código de la trama.

ACK: 0=aplicada, 1=contenido rechazado, 2=orden no soportada, 3=estado inválido.
CAL_RESP confirma calibración; en caso de error llega ACK de CAL_REQ con estado
no cero. Las demás órdenes requieren ACK con el tipo coincidente. Los nodos
pueden enviar EVENT/TICK_REPORT mientras el host espera la respuesta de una orden.

## Tareas y ciclo de vida

Formato ASCII `id,tipo,W,T,D,criticidad`, separado por comas, sin NUL:

- ID de 1 a 65535; 0 identifica CPU ociosa en los eventos.
- Tipo P o S; W, T y D positivos, uint32.
- Criticidad int32 con signo. Mayor valor significa mayor criticidad.
- Máximo ocho tareas por nodo y una esporádica.
- El host elimina CR/LF del CSV de entrada y transmite líneas con LF.

Secuencia: CAL_REQ/CAL_RESP → DEPLOY/ACK → START/ACK. El host envía DEPLOY
incluso al nodo sin tareas para borrar una tabla conservada de una sesión anterior.
Calibración y reemplazo de tabla se rechazan durante la ejecución. Una placa
que sigue ejecutándose tras desconectar el host debe recibir STOP voluntario o
reset físico antes de una sesión nueva. El host no detiene esa sesión al abrir.

START establece tick cero y reinicia contadores. Un nodo sin tareas también
emite reportes. En firmware, report_ticks=0 desactiva reportes; el host exige
1..65535 porque los necesita para detectar caídas. START repetido devuelve 3.
El ACK puede intercalarse con los eventos iniciales; el receptor despacha por tipo.

ADD_TASK valida y agrega sin reiniciar el reloj ni alterar otros trabajos.
La primera liberación periódica ocurre en el siguiente tick local a procesar;
una esporádica espera botón/SPORADIC. TASK_ADDED informa la ranura asignada.
Los IDs duplicados se rechazan; el host no retransmite operaciones cuyo ACK se
pierde. Las tareas agregadas se conservan después de STOP, hasta otro DEPLOY.
El host utiliza ADD_TASK tanto para modo manual como para migración.

SPORADIC registra una pulsación. ACK 0 confirma el registro; Release o
SporadicIgnored informa si el calendarizador la aceptó según T mínimo.
`trigger ID` del host localiza el nodo por ID y envía una carga vacía.
Botón físico y debounce permanecen en el nodo.

STOP termina la ejecución y vacía los eventos pendientes antes de ACK 0.
STOP estando detenido devuelve 3. Tras su ACK no deben llegar más eventos ni
reportes de esa ejecución. El nodo conserva la tabla. Desconexión o silencio
del host no equivalen a STOP.

## Eventos

Carga de EVENT: `[cantidad:u8][registro × cantidad]`. Cada registro:

```text
[tipo:u8][tick:u32 LE][id:u16 LE][arg:u16 LE]
```

| Código | Evento | Significado de arg |
|---|---|---|
| 1 | RELEASE | Número de trabajo, 16 bits bajos |
| 2 | SWITCH | ID anterior; ID nuevo en el campo id, 0=ocioso |
| 3 | COMPLETE | Número de trabajo; tick es el final del último tramo |
| 4 | MISS | Número de trabajo abortado |
| 5 | SPORADIC_IGNORED | Motivo: 0=intervalo mínimo, 1=no hay esporádica |
| 6 | TASK_ADDED | Ranura 1..8 |
| 7 | TICK_LOST | Cantidad de ticks no procesados, saturada a 65535 |
| 8 | BAD_FRAME | Reservado; ambos firmwares cuentan tramas malas en TICK_REPORT |

SWITCH en k selecciona la tarea desde [k,k+1). No lleva una duración. El host
reconstruye intervalos con SWITCH, RELEASE, COMPLETE y MISS. COMPLETE/MISS cierra
el trabajo aunque no llegue inmediatamente otro SWITCH; trabajos consecutivos
del mismo ID pueden ejecutarse sin un SWITCH entre ambos. El número de trabajo
permite correlacionar liberación y finalización, con vuelta cada 65536 trabajos.

Ante TICK_LOST no se dibuja ejecución sobre el hueco. Las pérdidas de eventos o
tramas marcan la traza incompleta; no se inventa el trabajo que falta. Tras una
pérdida se espera un SWITCH para volver a inferir ejecución. Sin STOP confirmado,
el último tramo puede quedar abierto: solo se dibujan límites observados.

## Reportes de tick

```text
[tick:u32]
[misses:u16][sporadic_ignored:u16][ticks_lost:u16]
[events_dropped:u16][bad_frames:u16]
[max_busy_us:u32][max_wake_latency_us:u32]
```

Los cinco contadores saturan en 65535. tick k indica que ya se procesó el tramo
[k,k+1). Los reportes comienzan en N y siguen sus múltiplos. Tras perder ticks
se envía un solo reporte y se avanza al siguiente múltiplo, sin ráfaga.

El ESP32 puede intercalar reportes entre lotes de eventos más antiguos. El host
actualiza el watchdog inmediatamente con CLOCK_MONOTONIC, sin exigir que el tick
del reporte sea posterior al último evento. No usa el reporte para adelantar el
cursor del Gantt mientras pueden faltar eventos anteriores. Los contadores y
los tiempos se conservan en events.csv y los contadores se muestran en HTML.

## Transporte, tiempos y errores

Un intercambio pendiente por driver. Sin retransmisión automática de comandos.
Tramas con CRC/longitud inválidos se descartan. El receptor local recupera la
marca de inicio y cuenta errores; el host vence tramas parciales después de 200 ms.
Los lotes EVENT se validan completos antes de publicar registros en la cola IPC.

El host espera 1 s tras abrir el puerto para permitir el arranque de placas que
se reinician con USB/DTR. El tiempo de espera de CAL_REQ es como mínimo
50*tick_us + 1 s. STOP y ADD_TASK tienen como mínimo tick_us + 1 s, para terminar
un tramo y, en STOP, vaciar la cola UART. timeout_ms puede aumentar esos límites.
No se fijan velocidades sintéticas en producción.

En Pico, las respuestas de control tienen una cola acotada y prioridad frente
a telemetría. Cada vuelta atiende un comando y un lote de eventos para dejar
progresar el timer. TICK_REPORT puede salir entre lotes para evitar que una cola
saturada impida al host recibir heartbeat. En ESP32, ejecución, recepción UART
y transmisión de eventos usan contextos separados.

Los drivers publican eventos por memoria compartida; reporte nunca lee serial.
El reporte genera events.csv, gantt.csv, tasks.csv y report.html. START se encola
para ambos nodos antes de esperar ACK: sus intervalos envío/ACK dan una cota de
desfase inicial, no una medición exacta del instante físico.

Las pruebas locales están en [TESTING.md](TESTING.md). La carga y verificación
con placas reales se describen en [HARDWARE_TEST.md](HARDWARE_TEST.md).
