// CALENDARIZADOR 
// Por ahora solo tareas periódicas. 

#include "scheduler/scheduler.h"

// Agrega un evento a la decisión del tick. El número de trabajo va en 16 bits.
static void add_event(TickDecision &decision, EventType type, uint32_t tick, uint32_t task_id, uint32_t value){
    if (decision.event_count == SCHED_MAX_EVENTS_PER_TICK) {
        return;
    }

    NodeEvent &event = decision.events[decision.event_count];
    event.type = type;
    event.tick = tick;
    event.task_id = (uint16_t)task_id;
    event.value = (uint16_t)(value & 0xFFFFU);
    decision.event_count++;
}

Scheduler::Scheduler(){
    count_ = 0;
    ut_per_tick_ = 0;
    running_id_ = NO_TASK;
    pending_presses_ = 0;
    sporadic_ignored_ = 0;
}

bool Scheduler::configure(const Task *tasks, size_t count, uint32_t ut_per_tick){
    if (count > MAX_TASKS ||
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
        states_[i].job_seq = 0;
        states_[i].release_tick = 0;
    }

    count_ = count;
    ut_per_tick_ = ut_per_tick;
    running_id_ = NO_TASK;

    // las pulsaciones anteriores a START no cuentan
    pending_presses_ = 0;
    sporadic_ignored_ = 0;
    return true;
}

TickDecision Scheduler::tick(uint32_t now){
    TickDecision decision;
    decision.task_id = NO_TASK;
    decision.units = 0;
    decision.completed = false;
    decision.missed_count = 0;
    decision.released_count = 0;
    decision.event_count = 0;

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
        add_event(decision, EventType::Miss, now, state.task.id, state.job_seq);
    }

    // Liberar las tareas periódicas que tocan en este tick
    for (size_t i = 0; i < count_; i++) {
        TaskState &state = states_[i];

        if (state.task.type != TaskType::Periodic ||
            now < state.next_release) {
            continue;
        }

        release_job(state, now, decision);

        // la próxima liberación mantiene la fase: si se saltaron ticks,
        // se avanza de a T hasta pasar el tick actual
        do {
            state.next_release += state.task.T;
        } while (state.next_release <= now);
    }

    // Pulsaciones de la tarea esporádica (después de las periódicas)
    handle_presses(now, decision);

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

    // si cambia la tarea en ejecución se avisa (arg = la anterior)
    const uint32_t chosen_id = (chosen != nullptr) ? chosen->task.id : NO_TASK;
    if (chosen_id != running_id_) {
        add_event(decision, EventType::Switch, now, chosen_id, running_id_);
        running_id_ = chosen_id;
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

    // terminó: la marca es el final de este tick
    if (chosen->remaining == 0) {
        chosen->pending = false;
        decision.completed = true;
        add_event(decision, EventType::Complete, now + 1, chosen->task.id, chosen->job_seq);
    }

    return decision;
}

size_t Scheduler::count() const{
    return count_;
}

void Scheduler::sporadic_press(uint32_t presses){
    // se topa en vez de dar la vuelta
    const uint32_t room = 0xFFFFFFFFU - pending_presses_;
    pending_presses_ += (presses < room) ? presses : room;
}

bool Scheduler::has_sporadic() const{
    for (size_t i = 0; i < count_; i++) {
        if (states_[i].task.type == TaskType::Sporadic) {
            return true;
        }
    }
    return false;
}

uint32_t Scheduler::sporadic_ignored() const{
    return sporadic_ignored_;
}

void Scheduler::release_job(TaskState &state, uint32_t now, TickDecision &decision){
    // la tarea anterior sigue sin terminar (con D > T o una esporádica
    // liberada de nuevo) y no se mantienen dos a la vez: se aborta
    if (state.pending) {
        decision.missed[decision.missed_count] = state.task.id;
        decision.missed_count++;
        add_event(decision, EventType::Miss, now, state.task.id, state.job_seq);
    }

    state.job_seq++;
    state.pending = true;
    state.remaining = state.task.W;
    state.deadline = now + state.task.D;
    state.release_tick = now;

    decision.released[decision.released_count] = state.task.id;
    decision.released_count++;
    add_event(decision, EventType::Release, now, state.task.id, state.job_seq);
}

void Scheduler::handle_presses(uint32_t now, TickDecision &decision){
    if (pending_presses_ == 0) {
        return;
    }

    const uint32_t presses = pending_presses_;
    pending_presses_ = 0;

    // como mucho hay una esporádica por nodo
    TaskState *sporadic = nullptr;
    for (size_t i = 0; i < count_; i++) {
        if (states_[i].task.type == TaskType::Sporadic) {
            sporadic = &states_[i];
            break;
        }
    }

    for (uint32_t p = 0; p < presses; p++) {
        // solo las primeras generan evento; el resto solo se cuenta
        const bool with_event = p < SCHED_MAX_PRESS_EVENTS_PER_TICK;

        if (sporadic == nullptr) {
            sporadic_ignored_++;
            if (with_event) {
                add_event(decision, EventType::SporadicIgnored, now, NO_TASK, SCHED_IGNORED_NO_TASK);
            }
            continue;
        }

        // la primera liberación siempre vale; después tienen que pasar
        // al menos T ticks desde la última liberación aceptada
        const bool allowed = sporadic->job_seq == 0 ||
                             now - sporadic->release_tick >= sporadic->task.T;

        if (allowed) {
            release_job(*sporadic, now, decision);
        } else {
            sporadic_ignored_++;
            if (with_event) {
                add_event(decision, EventType::SporadicIgnored, now, sporadic->task.id, SCHED_IGNORED_MIN_INTERVAL);
            }
        }
    }
}

