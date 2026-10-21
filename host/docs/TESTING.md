# Verificación del host

Pruebas ejecutadas en Linux sin hardware. El emulador Python usa PTY y velocidades
sintéticas; transmite trabajos de muestra para probar recepción y reporte.
**No ejecuta UT ni implementa/valida un calendarizador local RMS/EDF.**

## Ejecución

```sh
cd host
make test
ASAN_OPTIONS=detect_leaks=0 make sanitize
make clean
make
sha256sum -c docs/UT_SHA256SUMS
```

Compilación C11 con Wall, Wextra, Wpedantic, Wshadow, Wconversion y
Wstrict-prototypes, sin advertencias del código nuevo. Las pruebas de
interoperabilidad compilan los codecs originales con G++, sin SDK.

El primer intento con LeakSanitizer habilitado falló: su runtime no funciona
bajo ptrace en este entorno. Desactivando únicamente `detect_leaks`, la suite
pasó con AddressSanitizer y UndefinedBehaviorSanitizer. No se afirma haber
verificado fugas con LeakSanitizer. Repetir ese análisis en otra máquina.

## Cobertura automatizada

`tests/test_core.c` verifica:

- Redondeo C con divisiones exactas y UINT32_MAX, sin sumar W+s-1.
- EDF en uno y encima de uno, incluido exceso diminuto 1/(T1*T2); límite RMS
  y carga EDF admitida/RMS rechazada.
- Selección FF/WF distinta, velocidades invertidas y desempate por ID.
- Capacidad, esporádica existente y nodo no operativo.
- Criticidad negativa y límites int32, D<T/D>T, campos vacíos/extras, ceros,
  caracteres residuales, overflow y duplicados en archivo.
- CRC vector 0x29B1, tramas consecutivas/byte a byte, ruido pseudoaleatorio,
  corrupción, longitud excesiva, recuperación y fragmentos expirados.
- Dos procesos productores y consumidor, unicidad de registros, cola llena,
  timeout, contador de descartes y centinela que despierta consumidor bloqueado.
- Migración con carga previa fija: se conserva una tarea más crítica aunque
  una menos crítica tenga mayor utilización; no migran esporádicas.

`tests/test_interop.cpp` comprueba ida/vuelta byte por byte del codec C con
cada codec original ESP32/Pico, sin modificar sus fuentes.

`tests/integration.py` ejecuta el binario completo con nodos emulados:

1. Configuraciones desconocidas/repetidas y rangos inválidos; CSV duplicado,
   NUL y línea excesiva: salida 2 antes de abrir puertos o crear hijos.
2. Tres hijos del coordinador en `/proc`: dos drivers y reporte.
3. Inserción: un DEPLOY, un START y un ADD_TASK; sin STOP/START adicional.
4. Solicitud esporádica confirmada, CRC roto y lecturas fragmentadas.
5. CSV/HTML/Gantt desde RUN; latencias de primera ejecución y finalización
   separadas; contadores de plazo perdido y esporádicas ignoradas acumuladas.
6. `fail esp`: migración sin reiniciar el START del sobreviviente.
7. Nodo sin eventos de tareas sigue vivo por heartbeat; silenciar solo TICK
   detecta caída y provoca migración.
8. ADD_TASK aplicado con ACK perdido: estado incierto, una sola transmisión
   en ese nodo y ninguna repetición automática del efecto.
9. SIGTERM cierra host/reporte sin STOP y deja nodos ejecutando.
10. Consumidor suspendido y cola saturada: descartes explícitos, traza incompleta
    y heartbeat atendido sin migración espuria.
11. SIGKILL a un controlador: waitpid detecta falla y se migra.
12. Ambos nodos sin TICK: rechazos explícitos, ningún destino caído.
13. Esporádica del caído no migra; dos S se admiten en nodos distintos y una
    tercera se rechaza con motivo.
14. Firmware legacy sin CONFIGURE: nodo excluido antes de desplegar.
15. Calibración cero: nodo excluido sin división por cero.
16. STOP sin ACK: cierre acotado.
17. Reporte suspendido durante el cierre: salida 1 por reporte parcial, señales
    con límites y todos los hijos recogidos.
18. Migración por criticidad vía protocolo: retiene criticidad 9, rechaza
    criticidad 1 y conserva la tarea que ya ejecutaba el destino.
19. Ambos conjuntos de experimentos con las dos políticas correspondientes.

## Experimento EDF/RMS — sintético

`examples/edf_vs_rms.csv`; calibraciones **ESP32=10, Pico=10 UT/tick**,
tick 10 ms y First-Fit. Costos 5,5,4,4 y períodos/plazos 10 ticks.

| Algoritmo | ESP32 | Pico | Rechazos |
|---|---|---|---|
| EDF | IDs 1,2; U=1 | IDs 3,4; U=0.8 | ninguno |
| RMS | ID 1; U=0.5 | ID 2; U=0.5 | IDs 3,4 |

RMS rechaza 0.5+0.4=0.9 porque n=2 da 2(sqrt(2)-1) ≈ 0.828427.
EDF admite la carga completa; la prueba RMS del proyecto solo admite parte.
Esto verifica admisión, no comportamiento temporal físico.

## Experimento First-Fit/Worst-Fit — sintético

`examples/ff_vs_wf.csv`; **ESP32=10, Pico=5 UT/tick**, EDF, tick 10 ms.
Primera tarea W=40,T=10: C=4/8, U=0.4/0.8. Segunda W=10,T=10:
C=1/2, U=0.1/0.2.

| Heurística | ESP32 | Pico |
|---|---|---|
| First-Fit | IDs 1,2; U=0.5 | vacía; U=0 |
| Worst-Fit | ID 1; U=0.4 | ID 2; U=0.2 |

Worst-Fit coloca la primera en ESP32 (capacidad posterior 0.6 frente a 0.2).
Para la segunda, Pico queda con 0.8 libre frente a 0.5 del ESP32 y gana Pico.
**No se promete igual resultado con otras calibraciones.** Registrar valores
reales de s_n y volver a comparar con ellos.

## Pendiente con hardware

- Compilar/cargar firmwares completos con PlatformIO, ausente aquí.
- Ocho tareas por nodo, RMS/EDF y apropiación local al finalizar tick.
- Calibración, margen y estabilidad con carga USB/LEDs.
- Release/Run/Complete/Miss, pérdidas del búfer local e indicador de plazo.
- Botones, interrupciones, debounce, intervalo mínimo y latencias físicas.
- Inserción y migración sin alterar las demás tareas ni tick cero.
- Autonomía sin host/SIGTERM y diferencia respecto de STOP con q.
- Desfase real con GPIO/analizador frente a la cota de encolado/ACK.
- Comportamiento DTR/reset específico de cada placa al abrir/cerrar USB.
- Posible duplicación de ejecución entre nodo aislado y destino de migración.
- Respuestas TE con evidencia real del equipo; no se inventan aquí.
