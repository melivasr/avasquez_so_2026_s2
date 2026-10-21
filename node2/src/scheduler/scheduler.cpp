// CALENDARIZADOR 
// Por ahora solo tareas periódicas. 

#include "scheduler/scheduler.h"

Scheduler::Scheduler(){
    count_ = 0;
    ut_per_tick_ = 0;
}

bool Scheduler::configure(const Task *tasks, size_t count, uint32_t ut_per_tick){
    if (count == 0 ||
        count > MAX_TASKS ||
        ut_per_tick == 0) {
        return false;
    }

    // un período en 0 liberaría trabajos en cada tick para siempre
    for (size_t i = 0; i < count; i++) {
        if (tasks[i].T == 0) {
            return false;
        }
    }

    for (size_t i = 0; i < count; i++) {
        states_[i].task = tasks[i];
        states_[i].pending = false;
        states_[i].remaining = 0;
        states_[i].next_release = 0; // las periódicas liberan en el tick 0
        states_[i].deadline = 0;
    }

    count_ = count;
    ut_per_tick_ = ut_per_tick;
    return true;
}

TickDecision Scheduler::tick(uint32_t now){
    TickDecision decision;
    decision.task_id = NO_TASK;
    decision.units = 0;
    decision.completed = false;
    decision.missed_count = 0;
    decision.released_count = 0;

    // Abortar las tareas que llegaron a su plazo sin terminar.
    for (size_t i = 0; i < count_; i++) {
        TaskState &state = states_[i];

        if (!state.pending ||
            now < state.deadline) {
            continue;
        }

        state.pending = false;
        state.remaining = 0;
        decision.missed[decision.missed_count] = state.task.id;
        decision.missed_count++;
    }

    // Liberar las tareas periódicas que tocan en este tick
    for (size_t i = 0; i < count_; i++) {
        TaskState &state = states_[i];

        if (state.task.type != TaskType::Periodic ||
            now < state.next_release) {
            continue;
        }

        // solo pasa con D > T: la tarea anterior sigue sin terminar
        // y no se mantienen dos a la vez, así que se aborta como plazo perdido
        if (state.pending) {
            decision.missed[decision.missed_count] = state.task.id;
            decision.missed_count++;
        }

        state.pending = true;
        state.remaining = state.task.W;
        state.deadline = now + state.task.D;
        state.next_release = now + state.task.T;

        decision.released[decision.released_count] = state.task.id;
        decision.released_count++;
    }

    // Elegir la tarea pendiente de mayor prioridad
    TaskState *chosen = nullptr;
    for (size_t i = 0; i < count_; i++) {
        TaskState &state = states_[i];

        if (!state.pending) {
            continue;
        }
        if (chosen == nullptr ||
            has_priority(state, *chosen)) {
            chosen = &state;
        }
    }

    // nadie pendiente: tick de espera
    if (chosen == nullptr) {
        return decision;
    }

    // Ejecutar un tick de trabajo 
    uint32_t units = ut_per_tick_;
    if (chosen->remaining < units) {
        units = chosen->remaining;
    }
    chosen->remaining -= units;

    decision.task_id = chosen->task.id;
    decision.units = units;

    if (chosen->remaining == 0) {
        chosen->pending = false;
        decision.completed = true;
    }

    return decision;
}

size_t Scheduler::count() const{
    return count_;
}

