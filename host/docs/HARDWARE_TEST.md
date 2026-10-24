# Flujo para probar el host con ESP32 y Raspberry Pi Pico

El flujo es: **configurar → arrancar el host → calibrar → repartir tareas →
iniciar nodos → probar comandos → cerrar y revisar el reporte**.

El host y los dos firmwares usan ahora el [protocolo compartido](PROTOCOL.md).
Carga las versiones actualizadas de ambos nodos antes de probar: se añadió
ADD_TASK a Pico, criticidad int32 y DEPLOY vacío para limpiar tablas conservadas.
Las pruebas locales no confirman funcionamiento físico.

## 1. Conectar las placas y configurar los puertos

Conecta ambas placas con su firmware cargado. Identifica sus puertos y cierra
los monitores seriales de PlatformIO u otros programas que los estén usando.

Puedes identificar las placas conectando una primero, ejecutando lo siguiente
y repitiendo después de conectar la otra:

```bash
ls -l /dev/serial/by-id/
```

En [examples/host.ini](../examples/host.ini), coloca los puertos correspondientes:

```ini
esp_port=/dev/ttyUSB0
pico_port=/dev/ttyACM0
tasks=examples/tasks.csv
```

Los nombres de los puertos son ejemplos: utiliza los que detecte tu computadora.
También puedes usar las rutas completas de `/dev/serial/by-id/`.

Para comenzar, conserva la configuración de ejemplo:

```ini
algorithm=EDF
heuristic=First-Fit
tick_us=10000
report_ticks=10
missed_reports=3
timeout_ms=2000
output=report
```

El archivo [examples/tasks.csv](../examples/tasks.csv) contiene las tres tareas
iniciales. Las rutas de la configuración se interpretan desde el directorio
donde se ejecuta el host; los pasos siguientes lo ejecutan desde `host/`.

## 2. Arrancar el host

```bash
cd host
make
./build/host examples/host.ini
```

## 3. Observar el arranque automático

El host realiza estos pasos sin que tengas que enviar mensajes manualmente:

1. Lee el `.ini` y las tareas del CSV.
2. Crea los cuatro procesos: coordinador, controlador ESP32, controlador Pico
   y reporte.
3. Envía la duración del tick y solicita calibración a ambos nodos.
4. Recibe sus velocidades reales, expresadas en UT por tick.
5. Calcula costos, comprueba admisión y asigna tareas.
6. Envía DEPLOY a ambos nodos (vacío si no hay tareas) y espera sus ACK.
7. Envía START con algoritmo y período de reporte, estableciendo tick cero.

En pantalla debes ver las calibraciones y las tareas **desplegadas y
confirmadas**. Una tarea solamente **admitida** todavía está pendiente de
confirmación del nodo. Una operación **incierta** indica que se perdió la
confirmación y no se puede asegurar si el nodo la aplicó.

La distribución depende de la calibración real: no tiene que coincidir con
la obtenida mediante el emulador.

## 4. Comprobar el estado

En la consola del host, escribe:

```text
status
```

Debe mostrar los nodos operativos, sus velocidades, cantidad de tareas y
utilización. Espera las confirmaciones antes de pasar a la siguiente prueba.

Cada nodo debe enviar reportes de tick aunque no tenga tareas en ejecución.

## 5. Agregar una tarea durante la ejecución

```text
add 12,P,30,20,12,4
```

Este comando intenta agregar una tarea periódica con ID 12, trabajo de 30 UT,
periodo de 20 ticks, plazo relativo de 12 ticks y criticidad 4.

Debe confirmarse su despliegue o aparecer un rechazo con motivo. Las tareas
anteriores deben continuar y el tick cero debe mantenerse.

Consulta nuevamente:

```text
status
```

## 6. Probar la tarea esporádica

Pulsa el botón del nodo que recibió la tarea esporádica. En la carga de ejemplo
su ID es 3; puedes identificar el destino en los mensajes de despliegue.

También existe la solicitud de diagnóstico:

```text
trigger 3
```

El nodo decide si libera el trabajo o ignora el intento por intervalo mínimo.
El botón físico y su debounce también se gestionan en el nodo. Los eventos
recibidos deben quedar registrados en el reporte.

## 7. Probar una falla y la reubicación

Después de comprobar el funcionamiento normal, escribe:

```text
fail esp
```

Este comando envía STOP al ESP32 y espera confirmación antes de cerrar su
controlador y reubicar sus tareas. Debe detenerse su calendarizador y apagarse
los indicadores de tarea y plazo perdido. La placa permanece alimentada.
Si STOP no se confirma, el host avisa que no garantiza la detención y aun así
intenta reubicar las tareas. Una desconexión física de USB también provoca
reubicación, pero no permite enviar STOP al nodo retirado.

Espera los mensajes de reubicación y luego consulta:

```text
status
```

El resultado esperado es:

- Se conservan las tareas que ya tenía el nodo sobreviviente.
- Se intentan reubicar las periódicas del nodo caído si pasan admisión y caben.
- La esporádica del nodo caído se rechaza porque no se migra.
- Si no caben todas las periódicas, se aplica la política de criticidad.
- Las tareas reubicadas reinician sus trabajos; las demás mantienen su estado
  y el tick cero del sobreviviente no se reinicia.

Para simular la falla de la Pico, el comando equivalente es `fail pico`.

## 8. Terminar y revisar el reporte

```text
q
```

El host solicita detención a los nodos accesibles, espera confirmaciones con
tiempo límite, recoge los eventos finales y libera los recursos.

Desde la terminal, abre el reporte:

```bash
xdg-open report/report.html
```

La carpeta `report/` contiene:

- `report.html`: Gantt y resumen por nodo.
- `events.csv`: eventos, decisiones de admisión, rechazos, fallas y errores.
- `gantt.csv`: intervalos de ejecución observados.
- `tasks.csv`: estadísticas y latencias por tarea.

El Gantt se reconstruye de SWITCH, RELEASE, COMPLETE y MISS del firmware. Los intervalos faltantes no
se rellenan mediante una simulación del host.

## Comprobaciones físicas restantes

- Iniciar con los nodos detenidos o recién reiniciados; una sesión autónoma
  anterior rechaza calibración hasta recibir STOP voluntario o reset físico.
- Verificar RMS y EDF, ocho tareas por nodo y una esporádica por nodo.
- Comprobar botones, debounce, LEDs, calibración y margen con tráfico USB.
- Comprobar inserción y migración sin reinicio del sobreviviente.
- Alimentar aparte el nodo si se desconecta USB para probar autonomía.
- Medir desfase real y comparar con la cota de envío/ACK que registra el host.

El [contrato de mensajes](PROTOCOL.md) y las [pruebas locales](TESTING.md)
describen los formatos y los límites de lo validado sin placas.
