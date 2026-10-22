#ifndef SCHED_CORE_H
#define SCHED_CORE_H

// Núcleo del calendarizador local (RMS / EDF).
// Lógica pura: no incluye nada de ESP-IDF ni de FreeRTOS, para poder
// probarla en el computador. El temporizador, la tarea calendarizadora
// y la UART se conectan después mediante sched_tick() y los callbacks.

#include <stddef.h>
#include <stdint.h>
#include "../../../../common/rt_protocol.h"

#include "task.h"
#include "task_parser.h"   // MAX_TASKS

// Límite de id acordado en el diseño (cabe en 16 bits en los eventos).
constexpr uint32_t SCHED_MAX_TASK_ID = RT_MAX_TASK_ID;

enum class Algorithm : uint8_t {
    RMS = RT_RMS,
    EDF = RT_EDF
};

// Los valores coinciden con la tabla de eventos de README_NODO.md (9.5).
// Los demás tipos (SporadicIgnored, TaskAdded, TickLost, BadFrame) los
// emitirán otros módulos en pasos posteriores.
enum class SchedEventType : uint8_t {
    Release  = RT_RELEASE, // id = tarea, arg = n.º de trabajo
    Switch   = RT_SWITCH, // id = tarea que corre desde este tick (0 = ocioso), arg = anterior
    Complete = RT_COMPLETE, // id = tarea, arg = n.º de trabajo (tick = fin del último tramo)
    Miss     = RT_MISS, // id = tarea, arg = n.º de trabajo abortado
    SporadicIgnored = RT_SPORADIC_IGNORED // id = tarea esporádica (0 si no hay), arg = motivo SCHED_IGNORED_*
};

// Motivos de SporadicIgnored (campo arg).
constexpr uint16_t SCHED_IGNORED_MIN_INTERVAL = 0; // no pasó el intervalo mínimo
constexpr uint16_t SCHED_IGNORED_NO_TASK = 1;      // no hay tarea esporádica

// Pulsaciones que generan evento individual por tick; el resto solo se cuenta.
constexpr uint32_t SCHED_MAX_PRESS_EVENTS_PER_TICK = 8;

struct SchedEvent {
    SchedEventType type;
    uint32_t tick;
    uint16_t id;
    uint16_t arg;
};

// Reciben el contexto 'ctx' entregado en sched_init().
typedef void (*SchedEventSink)(const SchedEvent *event, void *ctx);
typedef void (*SchedWorkFn)(uint32_t ut, void *ctx); // ejecuta 'ut' unidades de trabajo

// Task.remaining se usa como UT restantes del trabajo activo.
struct SchedSlot {
    Task task;
    uint32_t cost_ticks;   // C = ceil(W / sn)
    uint32_t job_seq;      // n.º del trabajo más reciente (empieza en 1)
    uint32_t release_tick; // tick de liberación del trabajo activo
};

struct Scheduler {
    SchedSlot slots[MAX_TASKS];
    size_t count;
    Algorithm algorithm;
    uint32_t ut_per_tick;  // sn de la calibración
    uint32_t next_tick;    // próximo límite de tick a procesar (empieza en 0)
    uint16_t running_id;   // 0 = ocioso
    uint32_t miss_count;
    uint32_t pending_presses;   // pulsaciones registradas, aún sin procesar
    uint32_t sporadic_ignored;  // pulsaciones ignoradas desde sched_init
    SchedEventSink sink;   // puede ser nullptr
    SchedWorkFn work;      // puede ser nullptr
    void *ctx;
};

// Reinicia todo el estado. Falla si ut_per_tick == 0 o el algoritmo es inválido.
bool sched_init(Scheduler *s, Algorithm algorithm, uint32_t ut_per_tick,
                SchedEventSink sink, SchedWorkFn work, void *ctx);

// Agrega una tarea. Las periódicas se liberan por primera vez en s->next_tick.
// Las esporádicas nunca se liberan solas. No emite eventos.
// Falla si: tabla llena, id fuera de 1..65535, id repetido, W/T/D en 0,
// tipo inválido o segunda esporádica. slot_out (opcional) recibe 1..8.
bool sched_add_task(Scheduler *s, const Task *task, uint16_t *slot_out);

// Registra 'presses' pulsaciones (botón o SporadicRelease) para procesarlas en
// el próximo sched_tick. Se llama desde el mismo contexto que sched_tick.
void sched_sporadic_press(Scheduler *s, uint32_t presses);

// Procesa un límite de tick: plazos, liberaciones, pulsaciones de la
// esporádica, selección y un tramo.
void sched_tick(Scheduler *s);

#endif