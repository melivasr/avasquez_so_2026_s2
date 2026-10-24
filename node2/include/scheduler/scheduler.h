#pragma once

#include <stddef.h>
#include <stdint.h>

#include "event_queue.h"
#include "task.h"
#include "../../../common/rt_jobs.h"

// Pulsaciones de la esporádica que generan un evento cada una en un tick;
// las demás solo se cuentan
#define SCHED_MAX_PRESS_EVENTS_PER_TICK 8U

// Motivo de SPORADIC_IGNORED (va en arg)
#define SCHED_IGNORED_MIN_INTERVAL 0U // no pasó el intervalo mínimo T
#define SCHED_IGNORED_NO_TASK 1U      // el nodo no tiene tarea esporádica

// Eventos que puede generar un tick: hasta RT_MAX_JOBS plazos perdidos,
// MAX_TASKS liberaciones, las pulsaciones ignoradas, un cambio de tarea
// y una finalización
#define SCHED_MAX_EVENTS_PER_TICK (RT_MAX_JOBS + MAX_TASKS + SCHED_MAX_PRESS_EVENTS_PER_TICK + 2U)

// Lo que pasó y lo que hay que hacer en un tick
struct TickDecision {
    uint32_t task_id; // NO_TASK (0) si el nodo queda esperando
    uint32_t units; // UT a ejecutar en este tick
    bool completed; // el trabajo terminó en este tick

    // tareas cuyo trabajo se abortó 
    uint32_t missed[RT_MAX_JOBS];
    size_t missed_count;

    // tareas que liberaron un trabajo nuevo en este tick
    uint32_t released[MAX_TASKS];
    size_t released_count;

    // eventos para el host
    NodeEvent events[SCHED_MAX_EVENTS_PER_TICK];
    size_t event_count;
};

// Estado de cada tarea durante la ejecución
struct TaskState {
    Task task;
    bool pending; // hay un trabajo liberado sin terminar
    uint32_t remaining; // UT que le faltan al trabajo actual
    uint64_t next_release; // tick de la próxima liberación (periódicas)
    uint64_t release_tick; // tick en que se liberó el último trabajo
    uint64_t deadline; // plazo absoluto del trabajo pendiente más antiguo
    uint32_t job_seq; // número del último trabajo liberado (el primero es 1)
};

class Scheduler {
public:
    Scheduler();
    virtual ~Scheduler() {}

    // Carga una tabla válida, incluyendo la reserva de trabajos pendientes.
    // Un rechazo conserva la configuración anterior.
    bool configure(const Task *tasks, size_t count, uint32_t ut_per_tick);

    // Agrega sin reiniciar el reloj ni los trabajos existentes.
    bool add_task(const Task &task, uint32_t first_tick);

    // Procesa el tick now (ticks desde START): aborta las tareas que
    // llegaron a su plazo, libera las periódicas que tocan, atiende las
    // pulsaciones de la esporádica, elige la tarea de mayor prioridad y
    // le descuenta un tick.
    TickDecision tick(uint32_t now);

    // Registra pulsaciones de la esporádica (botón o SPORADIC_RELEASE);
    // se atienden en el próximo tick
    void sporadic_press(uint32_t presses);

    size_t count() const;

    // true si hay una tarea esporádica cargada
    bool has_sporadic() const;

    // Pulsaciones ignoradas desde configure (va en TICK_REPORT)
    uint32_t sporadic_ignored() const;

protected:
    // true si a debe correr antes que b
    virtual bool has_priority(const TaskState &a, const TaskState &b) const = 0;

private:
    // Encola un trabajo independiente, conservando los anteriores.
    void release_job(TaskState &state, uint64_t now, TickDecision &decision);

    // Atiende las pulsaciones pendientes de la esporádica
    void handle_presses(uint64_t now, TickDecision &decision);

    TaskState states_[MAX_TASKS];
    RtJobs jobs_;
    RtClock clock_;
    size_t count_;
    uint32_t ut_per_tick_;
    uint32_t running_id_; // tarea que corrió en el tick anterior (NO_TASK = ocioso)
    uint32_t pending_presses_; // pulsaciones sin atender
    uint32_t sporadic_ignored_; // pulsaciones ignoradas desde configure
};

