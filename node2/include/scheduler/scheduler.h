#pragma once

#include <stddef.h>
#include <stdint.h>

#include "event_queue.h"
#include "task.h"

// Lo que pasó y lo que hay que hacer en un tick
struct TickDecision {
    uint32_t task_id; // NO_TASK si el nodo queda esperando
    uint32_t units; // UT a ejecutar en este tick
    bool completed; // el trabajo terminó en este tick

    // tareas cuyo trabajo se abortó 
    uint32_t missed[MAX_TASKS];
    size_t missed_count;

    // tareas que liberaron un trabajo nuevo en este tick
    uint32_t released[MAX_TASKS];
    size_t released_count;
};

// Estado de cada tarea durante la ejecución
struct TaskState {
    Task task;
    bool pending; // hay un trabajo liberado sin terminar
    uint32_t remaining; // UT que le faltan al trabajo actual
    uint32_t next_release; // tick de la próxima liberación
    uint32_t deadline; // plazo absoluto: debe terminar antes de este tick
};

class Scheduler {
public:
    Scheduler();
    virtual ~Scheduler() {}

    // Carga las tareas. Devuelve false si count es mayor a MAX_TASKS,
    // si ut_per_tick es 0 o si alguna tarea tiene T en 0.
    bool configure(const Task *tasks, size_t count, uint32_t ut_per_tick);

    // Procesa el tick now (ticks desde START): aborta las tareas que
    // llegaron a su plazo, libera las que tocan, elige la tarea de mayor
    // prioridad y le descuenta un tick.
    TickDecision tick(uint32_t now);

    size_t count() const;

protected:
    // true si a debe correr antes que b
    virtual bool has_priority(const TaskState &a, const TaskState &b) const = 0;

private:
    TaskState states_[MAX_TASKS];
    size_t count_;
    uint32_t ut_per_tick_;
};

