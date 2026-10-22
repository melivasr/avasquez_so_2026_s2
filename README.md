# Calendarizador de tiempo real heterogéneo

Proyecto CE 4303: un host Linux distribuye tareas entre ESP32 y Raspberry Pi Pico H.
Cada nodo ejecuta su calendarizador RMS/EDF; el host realiza calibración, admisión,
particionado First-Fit/Worst-Fit, vigilancia, reubicación y reporte.

- [Host: compilación, configuración y comandos](host/README.md).
- [Contrato serial compartido](host/docs/PROTOCOL.md).
- [Pico: firmware y conexiones](node2/README.md).
- [ESP32: contrato y resultados históricos](nodo1ESP/ESP_RT_Timer/docs/CONTRATO.md).
- [Pruebas locales](host/docs/TESTING.md) y [prueba con placas](host/docs/HARDWARE_TEST.md).

```sh
make -C host
make -C host test
```

Para usar placas, compilar/cargar ambos firmwares actualizados y configurar sus
puertos en host/examples/host.ini. Desde host/, ejecutar:

```sh
./build/host examples/host.ini
```

Las constantes compartidas están en common/rt_protocol.h; conservar la estructura
del repositorio al compilar los proyectos PlatformIO.
