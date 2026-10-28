# Calendarizador de tiempo real heterogéneo

Proyecto CE 4303: un host Linux distribuye tareas entre ESP32 y Raspberry Pi Pico H.
Cada nodo ejecuta su calendarizador RMS/EDF; el host realiza calibración, admisión,
particionado First-Fit/Worst-Fit, vigilancia, reubicación y reporte.

- [Host: compilación, configuración y comandos](host/README.md).
- [Contrato serial compartido](host/docs/PROTOCOL.md).
- [Pico: firmware y conexiones](node2/README.md).
- [ESP32: contrato y resultados históricos](nodo1ESP/ESP_RT_Timer/docs/CONTRATO.md).
- [Pruebas locales](host/docs/TESTING.md) y [prueba con placas](host/docs/HARDWARE_TEST.md).

## Comandos esenciales

Ejecutar desde la raíz del repositorio. Para usar las placas, configurar sus
puertos en `host/examples/host.ini`, conectar ambas y cerrar los monitores
seriales. La compilación y carga de firmware requieren PlatformIO.

| Comando | Función |
|---|---|
| `make setup-usb` | Instala la regla udev para acceder a Pico en BOOTSEL. Requiere sudo; ejecutar una vez si la carga falla por permisos. |
| `make run` | Compila el host y ambos firmwares, carga ESP32 y Pico en secuencia e inicia el host. |
| `make host` | Compila el host si hace falta y lo inicia, usando el firmware ya cargado en las placas. `make run-host` es un alias. |
| `make` | Compila únicamente el host. |
| `make test` | Ejecuta las pruebas locales sin placas conectadas; requiere G++ y Python 3 además del compilador C. |

El host usa `examples/host.ini` por defecto. Se puede elegir otro INI mediante
`CONFIG`; esta ruta y las rutas internas del INI se interpretan desde `host/`:

```sh
make run CONFIG=examples/host.ini
make host CONFIG=examples/host.ini
```

Dentro del host, `q` detiene los nodos accesibles y genera el reporte en
`host/report/report.html`, junto con los CSV de eventos, Gantt y estadísticas.

## Experimentos

Cada experimento utiliza una carga de ocho tareas. Configurar los puertos en
los cuatro archivos `host/examples/experiment_*.ini` antes de ejecutarlos.
Con los firmwares actualizados, ejecutar una corrida a la vez:

```sh
# RMS contra EDF: misma carga y First-Fit; cambia el algoritmo.
make host CONFIG=examples/experiment_rms.ini
make host CONFIG=examples/experiment_edf.ini

# First-Fit contra Worst-Fit: misma carga y EDF; cambia la heurística.
make host CONFIG=examples/experiment_first_fit.ini
make host CONFIG=examples/experiment_worst_fit.ini
```

Las cuatro configuraciones incluyen `duration_s=30`: tras 30 segundos de
ejecución, el host detiene los nodos y genera los reportes automáticamente.
La calibración y el despliegue quedan fuera de ese tiempo; `q` permite terminar
antes. Esperar a que finalice cada corrida antes de iniciar la siguiente.

| Experimento | Reporte HTML desde la raíz |
|---|---|
| RMS | `host/report_experiment_rms/report.html` |
| EDF | `host/report_experiment_edf/report.html` |
| First-Fit | `host/report_experiment_first_fit/report.html` |
| Worst-Fit | `host/report_experiment_worst_fit/report.html` |

Cada carpeta también contiene los CSV. Repetir una configuración sobrescribe
su reporte anterior. Para cargar ambos firmwares antes de una corrida, usar
`make run` con el mismo `CONFIG`, por ejemplo:

```sh
make run CONFIG=examples/experiment_edf.ini
```

Los cálculos y resultados esperados según la calibración están en
[la guía de experimentos](host/docs/EXPERIMENTS.md).

Las constantes compartidas están en common/rt_protocol.h; conservar la estructura
del repositorio al compilar los proyectos PlatformIO.
