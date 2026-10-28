# Experimentos RMS/EDF y First-Fit/Worst-Fit

Las cargas usan como referencia la calibración física observada:
ESP32 = 70 UT/tick y Pico = 105 UT/tick, con `tick_us=10000` (10 ms).
El host vuelve a calibrar en cada ejecución; las velocidades no se fijan
en las configuraciones. Cada CSV contiene ocho tareas en total. Todas son
periódicas, con `D=T`.

## Ejecución

Con ambos firmwares actualizados y los monitores seriales cerrados, ejecutar
desde la raíz del repositorio, una comparación a la vez:

```sh
make host CONFIG=examples/experiment_rms.ini
make host CONFIG=examples/experiment_edf.ini

make host CONFIG=examples/experiment_first_fit.ini
make host CONFIG=examples/experiment_worst_fit.ini
```

`make host` inicia únicamente el host. Para compilar y cargar ambos firmwares
antes de una ejecución, usar `make run CONFIG=examples/experiment_rms.ini`
(o cualquiera de las otras configuraciones). Los puertos son `/dev/ttyUSB0`
y `/dev/ttyACM0`; ajustar los cuatro INI si cambian.

Los cuatro INI incluyen `duration_s=30`: cada corrida se detiene automáticamente
con STOP y genera el reporte tras 30 s desde el fin del arranque de los nodos,
sin contar compilación, carga, calibración ni despliegue. El cierre puede añadir
un tiempo breve. Se puede cambiar `duration_s` en los INI o usar `0` para duración
libre; `q` permite terminar antes.

Usar la misma duración en las cuatro corridas. No pulsar botones, agregar
tareas ni desconectar placas durante las mediciones. Esperar la salida del
host antes de iniciar la siguiente. Las salidas quedan en carpetas
distintas dentro de `host/`:

| Configuración | Reporte HTML |
|---|---|
| `experiment_rms.ini` | `report_experiment_rms/report.html` |
| `experiment_edf.ini` | `report_experiment_edf/report.html` |
| `experiment_first_fit.ini` | `report_experiment_first_fit/report.html` |
| `experiment_worst_fit.ini` | `report_experiment_worst_fit/report.html` |

Cada carpeta incluye además los CSV del reporte. Repetir una configuración
sobrescribe su salida anterior; guardar una copia si se harán varias réplicas.

## RMS contra EDF

Entrada: [`examples/edf_vs_rms.csv`](../examples/edf_vs_rms.csv).
Solo cambia `algorithm`; ambas configuraciones usan First-Fit.
Con las velocidades de referencia, `C=ceil(W/s_n)` da:

| ID | W (UT) | T = D (ticks) | C ESP32 (ticks) | C Pico (ticks) |
|---|---:|---:|---:|---:|
| 1 | 180 | 5 | 3 | 2 |
| 2 | 380 | 7 | 6 | 4 |
| 3 | 90 | 12 | 2 | 1 |
| 4 | 90 | 14 | 2 | 1 |
| 5 | 90 | 15 | 2 | 1 |
| 6 | 90 | 16 | 2 | 1 |
| 7 | 90 | 18 | 2 | 1 |
| 8 | 90 | 20 | 2 | 1 |

El orden de admisión es **2, 1, 3, 4, 5, 6, 7, 8**, por utilización decreciente en Pico,
el nodo más rápido. Se espera:

| Algoritmo | Tareas en ESP32 | U ESP32 | Tareas en Pico | U Pico | Rechazadas |
|---|---|---:|---|---:|---|
| EDF | 3, 4, 5, 6, 7, 8 | 1963/2520 ≈ 0.7790 | 2, 1 | 34/35 ≈ 0.9714 | Ninguna |
| RMS | 1, 5 | 11/15 ≈ 0.7333 | 2, 3, 4 | 61/84 ≈ 0.7262 | 6, 7, 8 |

EDF admite la carga completa porque ambas utilizaciones son ≤ 1. En RMS,
agregar la tarea 1 en Pico produciría `4/7 + 2/5 = 34/35`, que supera el
límite para dos tareas `2(√2−1) ≈ 0.8284`; por eso pasa a ESP32. Las tareas
3 y 4 caben en Pico, con utilización `4/7 + 1/12 + 1/14 = 61/84`.
La tarea 5 ya supera el límite RMS para cuatro tareas en Pico, pero cabe en
ESP32, que queda con `3/5 + 2/15 = 11/15`.

Las tareas 6, 7 y 8 se rechazan. Incluso la más liviana, la 8, produciría
`61/84 + 1/20 ≈ 0.7762` en Pico, superior al límite RMS para cuatro tareas
`4(2^(1/4)−1) ≈ 0.7568`; en ESP32 produciría
`11/15 + 2/20 ≈ 0.8333`, superior al límite para tres tareas
`3(2^(1/3)−1) ≈ 0.7798`.

La pareja de Pico en EDF también muestra una diferencia real de planificación:
si se forzaran ambas tareas en ese nodo bajo RMS, la tarea 1 ejecutaría en
los intervalos `[0,2)` y `[5,7)`, dejando solo tres de los cuatro ticks
necesarios para la tarea 2 antes de su plazo 7. EDF puede completarlas sin
vencimientos. **La corrida RMS del host no fuerza esa asignación**: aplica
admisión y rechaza tareas, por lo que se esperan cero MISS en sus tareas
admitidas. El resultado principal es **8 tareas admitidas con EDF frente a
5 con RMS**, con cero MISS en EDF si el HW mantiene los ticks.

## First-Fit contra Worst-Fit

Entrada: [`examples/ff_vs_wf.csv`](../examples/ff_vs_wf.csv).
Solo cambia `heuristic`; ambas configuraciones usan EDF.

| ID | W (UT) | T = D (ticks) | C ESP32 (ticks) | C Pico (ticks) |
|---|---:|---:|---:|---:|
| 1–4 | 180 | 16 | 3 | 2 |
| 5–8 | 90 | 16 | 2 | 1 |

El orden de admisión es **1, 2, 3, 4, 5, 6, 7, 8**: primero las de mayor
costo y, entre iguales, el ID menor. Ambas heurísticas colocan la tarea 1 en
Pico, donde consume `2/16 = 0.125`. Para la tarea 2, First-Fit vuelve a
elegir Pico porque aún cabe. Worst-Fit compara la capacidad restante tras
agregarla: `1−4/16 = 0.75` en Pico frente a `1−3/16 = 0.8125` en ESP32;
elige ESP32. En los empates posteriores de capacidad libre se prefiere el
nodo más rápido, Pico.

| Heurística | Tareas en ESP32 | U ESP32 | Tareas en Pico | U Pico |
|---|---|---:|---|---:|
| First-Fit | Ninguna | 0 | 1, 2, 3, 4, 5, 6, 7, 8 | 12/16 = 0.75 |
| Worst-Fit | 2, 5, 7 | 7/16 = 0.4375 | 1, 3, 4, 6, 8 | 8/16 = 0.5 |

Ambas deben admitir las ocho tareas y tener cero MISS. El Gantt debe mostrar
las ocho tareas en Pico con First-Fit, y cinco en Pico y tres en ESP32 con
Worst-Fit. First-Fit también permite comprobar ocho tareas desplegadas
simultáneamente en Pico. La utilización total cambia de 0.75 a 0.9375
porque las tareas 2, 5 y 7 cuestan más ticks en ESP32; es un efecto esperado
de la heterogeneidad.

## Qué comprobar en los resultados

- Calibración de cada corrida, asignaciones, tareas admitidas/rechazadas y
  utilización teórica frente a las tablas.
- Cero ticks perdidos, eventos descartados y errores de comunicación. Una
  corrida con pérdidas no permite atribuir los resultados solo al algoritmo.
- MISS, trabajos completados, latencias y ocupación del Gantt. Comparar tasas
  o la misma ventana de ticks si las duraciones no fueron idénticas.
- Los trabajos pendientes al terminar con `q` pueden explicar una diferencia
  entre liberaciones y completados sin constituir un vencimiento.

Estas asignaciones conservan los mismos costos para velocidades enteras de
ESP32 entre 64 y 75 UT/tick y Pico entre 95 y 124 UT/tick. Fuera de esos
rangos, recalcular `ceil(W/s_n)` con la calibración obtenida antes de usar las
tablas. Los resultados descritos son expectativas del modelo, no mediciones
de estas cuatro corridas en las placas.
