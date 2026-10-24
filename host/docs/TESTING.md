# Verificación de comunicación

Desde `host/`, en Linux con GCC, G++, Make y Python 3:

```sh
make test
ASAN_OPTIONS=detect_leaks=0 make sanitize
make clean
make
```

La suite no requiere pyserial, PlatformIO ni dispositivos seriales. Usa carpetas
temporales para sus reportes y PTY para conectar el host con dos instancias de
las fuentes reales de Pico compiladas en Linux. `tests/native/` sustituye solo
las interfaces de Arduino/USB, GPIO y alarma por implementaciones de prueba.
La calibración y las UT del firmware se ejecutan realmente sobre la CPU Linux;
sus velocidades no representan las placas. Las dos ranuras del host usan Pico
en esta prueba; el runtime ESP-IDF se comprueba por separado en sus módulos puros.

## Cobertura

- `test_core.c`: límites de ID y criticidad, admisión EDF/RMS, Worst-Fit,
  migración, CRC, START, tiempos de espera y decodificación atómica de lotes.
- `test_interop.cpp`: 60 tramas con cada codec original ESP32/Pico, tamaños
  hasta 512 bytes, CRC corrupto y TICK_REPORT/EVENT producidos por sus módulos
  originales y consumidos por el host. También valida START con el parser Pico.
- `test_report.c`: Gantt con SWITCH, trabajos consecutivos sin cambio de ID,
  heartbeat intercalado con eventos viejos, TICK_LOST y motivos de esporádicas.
- `test_pico.cpp`: criticidad int32, NUL/overflow, inserción local conservando
  progreso, primera liberación y rechazo de segunda esporádica/ID duplicado.
- `test_jobs.cpp`, compilado contra cada núcleo: trabajos periódicos y
  esporádicos solapados, FIFO en RMS/EDF, plazo grande agregado después del
  tick cero, vuelta del contador de 32 bits, capacidad y vencimientos en lote.
- El reporte comprueba latencias y Gantt de dos trabajos solapados del mismo
  ID. La integración verifica que un intento duplicado se registra aparte y
  la tarea original continúa ejecutándose con sus parámetros.
- Cuatro suites originales `ESP_RT_Timer/host_tests/*_test.cpp`: núcleo RMS/EDF,
  parser, cola y reporte de tick. Se incluye la criticidad negativa en el parser.
- `integration.py`: comandos del firmware nativo, tabla vacía, heartbeat sin
  tareas, ID 65535, ADD_TASK, disparo/intervalo mínimo, CRC malo, STOP con vaciado,
  arranque del host en RMS/EDF, inserción y migración sin otro START, rechazo de
  esporádica del caído, reportes CSV/Gantt, calibración que supera timeout_ms,
  continuidad de nodos tras SIGTERM, heartbeat con la cola de eventos saturada
  y entradas inválidas antes de abrir puertos.

`make sanitize` compila con AddressSanitizer y UndefinedBehaviorSanitizer. La
opción mostrada desactiva únicamente LeakSanitizer; no constituye prueba de fugas.
El adaptador nativo no prueba el SDK, ISR reales, USB físico, LEDs ni botones.

## Comparaciones

`examples/edf_vs_rms.csv` y `examples/ff_vs_wf.csv` siguen siendo entradas útiles
para experimentos. Sus resultados dependen de s_n medido. No se supone una
velocidad fija para ninguna placa ni se presentan las pruebas nativas como
experimentos físicos RMS/EDF o FF/WF.

## Validación con placas

Compilar y cargar ambos firmwares actualizados; cerrar monitores seriales y
seguir [HARDWARE_TEST.md](HARDWARE_TEST.md). Verificar botones/debounce, LEDs,
margen de calibración, ocho tareas por nodo, carga USB, pérdida de plazos,
inserción y migración, autonomía con alimentación independiente y desfase real.
