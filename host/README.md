# Host Linux — Calendarizador de Tiempo Real Heterogéneo

Implementación en C del host del Proyecto 1.

## Compilar y ejecutar

Requiere Linux, GCC/Clang, Make y glibc >= 2.30 (`sem_clockwait`). Python 3 y G++
solo se necesitan para pruebas. El ejecutable no tiene dependencia de Python.

```sh
cd host
make
# Editar los puertos/rutas de examples/host.ini.
./build/host examples/host.ini
```

Las rutas de configuración son relativas al directorio de ejecución. Los puertos
son 115200, 8N1, sin control de flujo, abiertos exclusivamente por su controlador.
El usuario debe tener acceso a los dispositivos seriales. No ejecutar dos hosts
sobre los mismos puertos.

**El firmware actual aún no permite una ejecución física completa.** El ESP32
responde calibración y despliegue; todavía debe implementar el resto del contrato.
La Pico tiene codecs/cola, pero su `main.cpp` solo hace blink. El host excluye un
nodo que no confirma CONFIGURE; el motivo queda en el reporte.
[Contrato e integración pendiente](docs/PROTOCOL.md).

## Configuración

| Clave | Valores/default |
|---|---|
| `algorithm` | `EDF` (default) o `RMS` |
| `heuristic` | `First-Fit` (default) o `Worst-Fit` |
| `tick_us` | 10000; rango 1000–1000000 µs, coincide con calibración ESP32 |
| `esp_port`, `pico_port` | Rutas distintas, obligatorias |
| `report_ticks` | 10; entero positivo |
| `missed_reports` | 3; entero positivo |
| `timeout_ms` | 2000; rango 100–60000 ms por intercambio serial |
| `tasks` | Archivo CSV obligatorio |
| `output` | `report`; directorio de salida, se sobrescriben sus cuatro reportes |

Claves desconocidas, repetidas, valores incompletos, NUL y líneas excesivas se
rechazan. La ventana de vigilancia `tick_us * report_ticks * missed_reports` no
puede exceder 24 h. Conviene usar una ventana mayor que el jitter USB esperado.
El ESP32 mide 50 ticks al calibrar: para ticks grandes, aumentar `timeout_ms`
por encima de esa ventana más el transporte. No se fijan velocidades reales.

## Tareas y comandos

CSV con columnas `id,tipo,W,T,D,criticidad`. Se acepta ese encabezado como primera
fila y también archivos sin encabezado. Se permiten comentarios que comienzan
con `#` y líneas vacías. Ejemplo:

```text
id,tipo,W,T,D,criticidad
3,P,40,20,20,2
4,S,10,50,15,9
```

`id` usa 1..4294967294 (0 y UINT32_MAX reservados), `W/T/D` son uint32 positivos,
criticidad es int32 con signo y `tipo` es `P` o `S`. Se conservan plazos menores,
iguales o mayores que T. Hay como máximo 512 IDs por sesión, incluidos rechazados;
no se reutilizan IDs. Como máximo ocho tareas desplegadas por nodo y una
esporádica por nodo. Las líneas de tareas tienen un límite de 158 caracteres.

Los tres CSV de `examples/` son **archivos de entrada**: cada fila describe una
tarea que el host intenta admitir y asignar a un nodo. La primera fila contiene
los nombres de las columnas. `uint32` es un entero
sin signo de 32 bits; `int32` es un entero con signo de 32 bits. Con el tick por
defecto, un tick equivale a 10 ms. La criticidad se usa ante fallas; las
prioridades locales siguen el algoritmo RMS o EDF seleccionado.

| Archivo | Para qué sirve |
|---|---|
| `examples/tasks.csv` | Carga inicial de muestra: dos tareas periódicas y una esporádica. Es la que utiliza `examples/host.ini` por defecto. |
| `examples/edf_vs_rms.csv` | Comparar admisión EDF/RMS: con velocidades sintéticas 10/10 UT por tick y First-Fit, EDF admite las cuatro tareas y RMS rechaza las tareas 3 y 4. |
| `examples/ff_vs_wf.csv` | Comparar asignación First-Fit/Worst-Fit: con velocidades sintéticas 10/5 UT por tick y EDF, First-Fit pone ambas tareas en ESP32 y Worst-Fit pone una en cada nodo. |

Para cargar otro conjunto, cambiar `tasks=examples/tasks.csv` en la configuración
por la ruta del CSV elegido. El archivo CSV describe las tareas; `algorithm` y
`heuristic` se eligen en la configuración. Los resultados de los ejemplos de
comparación dependen de las calibraciones indicadas. Los CSV generados en
`report/` son archivos de salida con eventos, intervalos y estadísticas.

```text
add 12,P,30,20,12,4
trigger 4
status
fail esp
fail pico
q
```

Los comandos salvo `q` terminan con Enter. En una terminal, `q` al comienzo de
línea actúa inmediatamente. `add` valida, admite y solicita ADD_TASK; espera ACK,
sin detener tareas ni reenviar START. `trigger` es una solicitud diagnóstica de
liberación esporádica; el nodo conserva la decisión de ignorarla por intervalo
mínimo. El botón físico, debounce e interrupción siguen perteneciendo al nodo.

`fail esp`/`fail pico` cierran el controlador correspondiente sin STOP y provocan
reubicación. También se puede desconectar USB. **Una falla de comunicación no
prueba que el procesador dejó de ejecutar:** si sigue alimentado, el nodo aislado
puede continuar tareas que también se reubican. El proyecto no define fencing
ni consenso distribuido; la aplicación no garantiza ejecución única ante una
partición de comunicación. Para experimentos sin duplicados, apagar el nodo
fallado o confirmar físicamente su detención.

`q` solicita STOP a controladores accesibles, espera ACK con límite, da 100 ms
para eventos finales y cierra procesos. SIGINT/SIGTERM finalizan el host y su
reporte **sin STOP**: los nodos deben continuar. EOF de stdin deshabilita la
entrada y mantiene la supervisión. SIGKILL no permite limpieza normal del padre;
los drivers reciben señal de muerte del padre y el reporte detecta la orfandad.
La memoria compartida se desvincula inmediatamente después de mmap para evitar
nombres huérfanos. El host limpia con waitpid, cierre de descriptores, sem_destroy
y munmap en las salidas controladas.

## Módulos y teoría para la defensa

| Archivo | Responsabilidad |
|---|---|
| `src/main.c` | Coordinador, ciclo de vida, despliegue/manual, supervisión y reubicación |
| `src/config.c`, `src/tasks.c` | Entradas acotadas y validación completa |
| `src/scheduler.c` | Costos, admisión, orden, First-Fit, Worst-Fit y plan de migración |
| `src/ipc.c` | shm_open/mmap, colas y semáforos compartidos |
| `src/protocol.c`, `src/serial.c` | Tramas/CRC/parser y transporte serial |
| `src/driver.c` | Proceso dueño de un puerto, transacciones y recepción de telemetría |
| `src/report.c` | Proceso consumidor, CSV y HTML/SVG |
| `include/host.h` | Estructuras e interfaces, sin punteros privados en memoria compartida |
| `tests/` | Pruebas de C, codecs originales y emulador PTY separado |
| `examples/` | Configuración y cargas de experimentos |

Se crean cuatro procesos reales con fork: coordinador, dos controladores y
reporte. El aislamiento separa transporte, decisiones de asignación y escritura
de reportes. Las colas de comandos, respuestas y eventos están en una región
POSIX compartida; no se usan variables ordinarias para transmitir estado entre
procesos. `eventfd` solo despierta al driver; los datos van por las colas.
`signalfd` evita trabajo inseguro en manejadores de señales.

Cada cola tiene 128 registros y tres semáforos pshared: espacios, elementos y
mutex. Productor: reservar espacio → tomar mutex → copiar/actualizar índice →
soltar mutex → publicar elemento. Consumidor: reservar elemento → tomar mutex →
copiar/actualizar índice → soltar mutex → devolver espacio. El lock de estado
nunca se adquiere dentro del mutex de una cola. No se anidan mutex de colas.
Ninguna región crítica contiene serial, archivos, ACK o una espera de otro
recurso. Se evitan así las dependencias circulares de deadlock. Las reservas
se devuelven si no puede adquirirse mutex. Las esperas usan reloj monotónico.

Los productores de eventos no esperan espacio: si la cola está llena, descartan
el registro y aumentan un contador compartido. La supervisión recibe el heartbeat
en el estado compartido antes de encolar su copia para el reporte. Una cola
saturada no bloquea esa actualización. Los descartes se muestran en HTML y se
marca **toda la traza del nodo como incompleta**: no se puede saber el intervalo
exacto perdido. Los mutex tienen espera máxima de 20 ms; no se usan spinlocks.
Las respuestas/comandos también tienen esperas acotadas. El cierre utiliza
R_EXIT/R_END y waitpid; si un hijo no termina, escala a SIGTERM/SIGKILL con límite.
Los semáforos POSIX no son robustos ante muerte en una región crítica; un timeout
impide el bloqueo indefinido, pero esa sesión puede perder información.

La supervisión se comprueba cada 100 ms y también al esperar respuestas cada
50 ms. Detecta ausencia de TICK con CLOCK_MONOTONIC, incluso cuando no existen
eventos de tareas. El instante de detección puede retrasarse por esa granularidad
y la carga de Linux; el host no pretende ser un sistema de tiempo real duro.
La reubicación se inicia después del intercambio en curso, siempre acotado.

No se encontraron PDF de Procesos, Deadlock o Planificación. La explicación usa
los criterios del enunciado y de la solicitud; no presupone lectura de lecciones.

## Admisión, asignación y migración

`C = W/s + (W%s != 0)` evita el desbordamiento de `W+s-1`. El nodo de mayor
calibración determina el orden decreciente C/T. El desempate es ID ascendente.
Las comparaciones del orden usan productos uint64 exactos.

First-Fit recorre velocidades decrecientes; empate: ESP32 antes de Pico.
Worst-Fit maximiza capacidad normalizada posterior `1 - suma(C/T)` entre nodos
admisibles; empate numérico: el mismo orden de velocidad/índice. No hay preferencia
fija por el tipo de microcontrolador cuando las calibraciones difieren.

La admisión incluye a la candidata y a esporádicas con T mínimo: EDF suma C/T ≤ 1;
RMS suma C/T ≤ n(2^(1/n)-1). EDF compara fracciones con enteros de nueve palabras
para no admitir una suma apenas superior a uno por redondeo; RMS usa long double.
No se sustituyen estas pruebas por análisis más permisivos. **Son los criterios
del proyecto, no una garantía general para plazos arbitrarios**; especialmente
EDF con D<T requiere otros análisis para una garantía general. El host conserva
D y reporta misses reales. RMS y EDF se ejecutan en los nodos: RMS da prioridades
fijas por período; EDF usa plazos absolutos dinámicos. El host únicamente
particiona; nunca indica qué ejecutar por tick y no enlaza ni ejecuta UT.

En una caída se fija la carga previa del sobreviviente. Se descartan esporádicas,
y se intenta el resto por utilización. Si no cabe todo, se elimina un candidato
de criticidad mínima y se repite desde la carga fija. Empate de criticidad:
se retira primero el ID mayor. Solo se despliega el conjunto final, recalculando
C con la calibración del destino. Una migración reinicia los trabajos de esa
tarea, sin alterar los del sobreviviente ni su tick cero. Ante un fallo de
comunicación durante esta fase, los resultados inciertos se registran y el nodo
se excluye. No se recupera progreso ni se migra automáticamente al nodo recuperado.

## Reportes y precisión

Se generan `events.csv`, `gantt.csv`, `tasks.csv` y `report.html`. Abrir el HTML
localmente. El reporte solo consume la cola compartida, nunca el serial.
`events.csv` incluye decisiones de admisión, rechazos, confirmaciones, estados
inciertos, errores, fallas, calibraciones, solicitudes y todos los eventos reales.
`tasks.csv` separa tiempo hasta primera ejecución y hasta finalización; conserva
período, plazo, costo, criticidad, releases, misses y esporádicas ignoradas.
`gantt.csv` y SVG solo contienen intervalos RUN observados. No se rellenan huecos
con simulación. Sin datos se escribe «sin datos», no utilización cero medida.

La utilización teórica es suma C/T de despliegues confirmados; cada confirmación
y retiro está registrado. La medida es ticks RUN observados / horizonte de ticks
observado desde START (cada nodo por separado). Con pérdidas es parcial, no una
estimación reconstruida. Se cuentan entradas/cambios de tarea, incluida entrada
desde idle/huecos. Los eventos FrameError/EventsLost marcan la traza incompleta.
Las medias de latencia solo usan pares correlacionables; releases solapados de
un mismo ID no se interpretan como trabajos distintos sin un identificador de
trabajo en el protocolo. Ver precisiones en docs/PROTOCOL.md.

START se encola para ambos drivers antes de esperar ACK. Se registra para cada
nodo el intervalo entre encolado en host y recepción de ACK. Suponiendo ACK después
de establecer cero, ese intervalo contiene el instante de START. La diferencia
entre intervalos da una cota de desfase; incluye IPC, USB, firmware y recepción.
No se presenta la llegada de ACK como instante exacto de ejecución, ni se alinean
artificialmente las dos trazas. Hay que validar esa cota con hardware/analizador.

## Pruebas y experimentos

```sh
make test
# En este entorno LeakSanitizer no funciona bajo ptrace:
ASAN_OPTIONS=detect_leaks=0 make sanitize
# Regresar al binario normal después de sanitizer:
make clean
make
# Integridad UT, desde host/:
sha256sum -c docs/UT_SHA256SUMS
```

Ver [pruebas y experimentos](docs/TESTING.md). Las velocidades sintéticas nunca
entran al ejecutable de producción. El emulador solo existe en `tests/`.

Para una sesión manual simulada:

```sh
python3 tests/emulator.py --speeds 10 5
# En otra terminal: copiar examples/host.ini a build/pty.ini,
# sustituir esp_port/pico_port por los PTY impresos y ejecutar:
./build/host build/pty.ini
```

Pendientes físicos: completar ambos firmwares, compilar/cargar con sus SDK,
calibrar en cada ejecución, observar LEDs/botones/debounce, pérdidas de plazos,
USB bajo carga, autonomía sin host y desfase real. PlatformIO no está instalado
en este entorno y no se hicieron cargas ni compilaciones de firmware completo.
El documento de atributo TE requiere respuestas reales del equipo; este host
no inventa participación ni resultados de experimentos físicos.
