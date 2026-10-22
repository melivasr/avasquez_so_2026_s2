# R2: inyección de fallos para `TICK_LOST`

**El firmware entregado NO contiene este código.** Este archivo guarda el parche para reproducir la prueba si hace falta.

## Resultado (tick de 10 ms, EDF, `N` = 100, 1000 ticks)

`test_tick_lost.py`: **15 de 15**.

- `TICK_LOST` recibidos: `(7, 153, 0, 2)` y `(7, 620, 0, 39)`, los esperados.
- Flujo de 1255 eventos idéntico al modelo con saltos de tiempo.
- Reportes en los ticks 100, 200, 300, 400, 500, **620**, 700 (ninguno en el 600: un solo reporte tras el salto, sin ráfaga).
- `ticks_perdidos` acumulado: 0, 2 y 41 en los reportes correspondientes.
- Ritmo real conservado; el bloqueo no cuenta en `max_ocupado_us` (7921 µs, 79 %).
- Efecto observado: `max_latencia_us` = 8441 µs en el último reporte. Tras un tick perdido, la latencia del despertar refleja el retraso respecto a la última alarma.

## Cómo se hizo

Se bloquea la tarea calendarizadora tras procesar ciertos ticks para que la ISR cuente ticks que nadie atiende. Se hace con una bandera de compilación en un entorno aparte.

### 1. `platformio.ini`

```ini
[platformio]
default_envs = esp32dev

[env:esp32dev]
platform = espressif32
board = esp32dev
framework = espidf
monitor_speed = 115200

; SOLO para la prueba R2 (inyección de fallos). Nunca se entrega con este entorno.
[env:esp32dev_fault]
extends = env:esp32dev
build_flags = -DRT_FAULT_INJECT
```

### 2. `src/node_runtime.cpp`, constantes (después de `TICK_US_MAX`)

```cpp
#ifdef RT_FAULT_INJECT
// SOLO PARA LA PRUEBA R2 (entorno esp32dev_fault; nunca se entrega así).
// Tras procesar el tick indicado se bloquea la tarea calendarizadora durante N,05
// ticks para que la ISR cuente ticks que no se atienden (TICK_LOST). El ,05 deja
// margen para que la cantidad de ticks perdidos sea determinista.
#include "esp_rom_sys.h"
static constexpr uint32_t FAULT_STALL_1_TICK = 150;
static constexpr uint32_t FAULT_STALL_1_CENTI = 305;   // 3,05 ticks -> se pierden 2
static constexpr uint32_t FAULT_STALL_2_TICK = 580;
static constexpr uint32_t FAULT_STALL_2_CENTI = 4005;  // 40,05 ticks -> se pierden 39
#endif
```

### 3. `src/node_runtime.cpp`, al final de `handle_tick()` (después de `event_tx_kick();`)

```cpp
#ifdef RT_FAULT_INJECT
    // Espera activa a propósito, solo en esta prueba (no existe en producción).
    if (tick == FAULT_STALL_1_TICK || tick == FAULT_STALL_2_TICK) {
        const uint32_t centi = (tick == FAULT_STALL_1_TICK) ? FAULT_STALL_1_CENTI
                                                            : FAULT_STALL_2_CENTI;
        esp_rom_delay_us(static_cast<uint32_t>(
            static_cast<uint64_t>(g_tick_us) * centi / 100));
    }
#endif
```

## Cómo ejecutarlo

1. Seleccionar el entorno `esp32dev_fault`, Build y Upload.
2. `~/venvs/ce4303/bin/python host_tests/test_tick_lost.py`
3. Volver al entorno `esp32dev`, Clean, Build y Upload antes de usar o entregar la placa.

## Por qué el número de ticks perdidos es determinista

El bloqueo dura `n + 0,05` ticks y empieza tras procesar el tick `k`. La ISR cuenta `n` alarmas durante el bloqueo mientras el tick bloqueado ocupe menos del 95 % del periodo (el nodo mide ~80 %). Al reanudar, el nodo procesa el tick `k + n` y declara `n − 1` ticks perdidos (los ticks `k + 1` a `k + n − 1`).