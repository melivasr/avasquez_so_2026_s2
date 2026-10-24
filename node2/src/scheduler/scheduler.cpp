// CALENDARIZADOR 
// Tareas periódicas y esporádicas; apropiación a nivel de tick.

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

    if (count && !tasks) return false;
    uint64_t reserved = 0;
    for (size_t i = 0; i < count; i++) {
        const Task &t = tasks[i];
        if (!t.id || t.id > RT_MAX_TASK_ID || !t.W || !t.T || !t.D ||
            (t.type != TaskType::Periodic && t.type != TaskType::Sporadic)) return false;
        for (size_t j = 0; j < i; ++j)
            if (tasks[j].id == t.id || (t.type == TaskType::Sporadic &&
                tasks[j].type == TaskType::Sporadic)) return false;
        reserved += rt_job_reservation(t.T, t.D);
    }
    if (reserved > RT_MAX_JOBS) return false;
    jobs_.clear();
    clock_ = RtClock{};

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

bool Scheduler::add_task(const Task &task, uint32_t first_tick){
    if (count_ >= MAX_TASKS || ut_per_tick_ == 0 || task.id == 0 ||
        task.id > RT_MAX_TASK_ID || task.W == 0 || task.T == 0 || task.D == 0 ||
        (task.type != TaskType::Periodic && task.type != TaskType::Sporadic)) {
        return false;
    }
    for (size_t i = 0; i < count_; ++i) {
        if (states_[i].task.id == task.id ||
            (task.type == TaskType::Sporadic && states_[i].task.type == TaskType::Sporadic)) {
            return false;
        }
    }
    uint64_t reserved = rt_job_reservation(task.T, task.D);
    for (size_t i = 0; i < count_; ++i)
        reserved += rt_job_reservation(states_[i].task.T, states_[i].task.D);
    if (reserved > RT_MAX_JOBS) return false;
    TaskState &state = states_[count_++];
    state = TaskState{};
    state.task = task;
    state.next_release = clock_.peek(first_tick);
    return true;
}

TickDecision Scheduler::tick(uint32_t raw_tick){
    const uint64_t now = clock_.observe(raw_tick);
    TickDecision decision;
    decision.task_id = NO_TASK;
    decision.units = 0;
    decision.completed = false;
    decision.missed_count = 0;
    decision.released_count = 0;
    decision.event_count = 0;

    // Cada trabajo vence por su propio plazo, aunque haya otros del mismo ID.
    for (auto &job : jobs_.jobs) {
        if (!job.active || now < job.deadline) continue;
        const Task &task = states_[job.slot].task;
        job.active = false;
        decision.missed[decision.missed_count++] = task.id;
        add_event(decision, EventType::Miss, raw_tick, task.id, job.sequence);
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
        state.next_release += ((now - state.next_release) / state.task.T + 1) * state.task.T;
    }

    // Pulsaciones de la tarea esporádica (después de las periódicas)
    handle_presses(now, decision);

    // Elegir la tarea pendiente de mayor prioridad
    TaskState *chosen = nullptr;
    for (size_t i = 0; i < count_; i++) {
        TaskState &state = states_[i];

        RtJob *head = jobs_.first(i);
        state.pending = head != nullptr;
        if (!head) continue;
        state.remaining = head->remaining;
        state.deadline = head->deadline;
        if (chosen == nullptr ||
            has_priority(state, *chosen)) {
            chosen = &state;
        }
    }

    // si cambia la tarea en ejecución se avisa (arg = la anterior)
    const uint32_t chosen_id = (chosen != nullptr) ? chosen->task.id : NO_TASK;
    if (chosen_id != running_id_) {
        add_event(decision, EventType::Switch, raw_tick, chosen_id, running_id_);
        running_id_ = chosen_id;
    }

    // nadie pendiente: tick de espera
    if (chosen == nullptr) {
        return decision;
    }

    // Los trabajos de un mismo ID se ejecutan en orden de liberación.
    RtJob *job = jobs_.first((size_t)(chosen - states_));
    uint32_t units = ut_per_tick_;
    if (chosen->remaining < units) {
        units = chosen->remaining;
    }
    chosen->remaining -= units;
    job->remaining -= units;

    decision.task_id = chosen->task.id;
    decision.units = units;

    // terminó: la marca es el final de este tick
    if (chosen->remaining == 0) {
        job->active = false;
        chosen->pending = jobs_.first((size_t)(chosen - states_)) != nullptr;
        decision.completed = true;
        add_event(decision, EventType::Complete, raw_tick + 1, chosen->task.id, job->sequence);
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

void Scheduler::release_job(TaskState &state, uint64_t now, TickDecision &decision){
    RtJob *job = jobs_.push((size_t)(&state - states_), state.job_seq + 1,
                            now, state.task.D, state.task.W);
    if (!job) return; // La reserva de la tabla se comprobó antes de desplegar.
    state.job_seq++;
    state.release_tick = now;
    decision.released[decision.released_count++] = state.task.id;
    add_event(decision, EventType::Release, (uint32_t)now, state.task.id, state.job_seq);
}

void Scheduler::handle_presses(uint64_t now, TickDecision &decision){
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
                add_event(decision, EventType::SporadicIgnored, (uint32_t)now, NO_TASK, SCHED_IGNORED_NO_TASK);
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
                add_event(decision, EventType::SporadicIgnored, (uint32_t)now, sporadic->task.id, SCHED_IGNORED_MIN_INTERVAL);
            }
        }
    }
}

