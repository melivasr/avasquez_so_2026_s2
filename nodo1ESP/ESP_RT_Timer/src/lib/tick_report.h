#ifndef TICK_REPORT_H
#define TICK_REPORT_H

// Carga útil de la trama TickReport y regla de periodicidad.
// Lógica pura (sin ESP-IDF).
//
// Formato (22 bytes, little endian):
//   [tick: 4][plazos_perdidos: 2][esporadicas_ignoradas: 2][ticks_perdidos: 2]
//   [eventos_descartados: 2][tramas_malas: 2][max_ocupado_us: 4][max_latencia_us: 4]
// Los contadores de 16 bits saturan en 65535.

#include <stddef.h>
#include <stdint.h>
#include "../../../../common/rt_protocol.h"

constexpr size_t TICK_REPORT_SIZE = RT_TICK_REPORT_SIZE;

struct TickReportData {
    uint32_t tick;              // último tick procesado
    uint32_t deadline_misses;
    uint32_t sporadic_ignored;
    uint32_t ticks_lost;
    uint32_t events_dropped;
    uint32_t bad_frames;
    uint32_t max_busy_us;       // mayor tiempo de proceso dentro de un tick
    uint32_t max_wake_latency_us; // mayor retraso entre la alarma y el despertar
};

// Escribe TICK_REPORT_SIZE bytes. Devuelve esa longitud, o 0 si out es nulo
// o out_capacity es menor.
size_t tick_report_encode(const TickReportData &data, uint8_t *out, size_t out_capacity);

// ¿Toca enviar un reporte al procesar 'tick'? period = N (0 = desactivado).
// *next_report es el próximo tick de envío (al iniciar: N). Si toca, lo
// avanza al siguiente múltiplo de N posterior a 'tick' (así un salto de ticks
// perdidos no produce ráfagas). No contempla desbordar los 32 bits de tick.
bool tick_report_due(uint32_t period, uint32_t tick, uint32_t *next_report);

#endif