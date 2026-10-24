#include "lib/sched_core.h"

// Orden dentro de cada tick (k = s->next_tick):
//   1. Plazos: trabajo activo con plazo absoluto <= k -> MISS y aborto.
//   2. Liberaciones periódicas: se encolan sin reemplazar trabajos pendientes.
//   3. Pulsaciones de la esporádica: cada una libera un trabajo solo si
//      pasó el intervalo mínimo T desde la última liberación; si no, se
//      ignora (SPORADIC_IGNORED) y se cuenta.
//   4. Selección según RMS o EDF.
//   5. SWITCH si cambia la tarea en ejecución.
//   6. Ejecución de un tramo: min(sn, UT restantes). Si termina -> COMPLETE
//      con marca k+1.

static void emit(Scheduler *s, SchedEventType type, uint32_t tick,
                 uint16_t id, uint16_t arg)
{
    if (s->sink == nullptr) {
        return;
    }
    const SchedEvent event = { type, tick, id, arg };
    s->sink(&event, s->ctx);
}

// true si 'a' tiene mayor prioridad que 'b'.
// RMS: T menor. EDF: plazo absoluto menor.
// Desempate (ambos): criticidad mayor, luego id menor.
static bool higher_priority(const Scheduler *s, const SchedSlot &a, const SchedSlot &b)
{
    uint64_t key_a;
    uint64_t key_b;

    if (s->algorithm == Algorithm::RMS) {
        key_a = a.task.T;
        key_b = b.task.T;
    } else {
        key_a = a.task.absolute_deadline;
        key_b = b.task.absolute_deadline;
    }

    if (key_a != key_b) {
        return key_a < key_b;
    }
    if (a.task.criticality != b.task.criticality) {
        return a.task.criticality > b.task.criticality;
    }
    return a.task.id < b.task.id;
}

static void abort_job(Scheduler *s, RtJob &job, uint32_t tick)
{
    emit(s, SchedEventType::Miss, tick,
         static_cast<uint16_t>(s->slots[job.slot].task.id),
         static_cast<uint16_t>(job.sequence));
    s->miss_count++;
    job.active = false;
}

// Cada liberación conserva su plazo, trabajo restante y número de trabajo.
static void release_job(Scheduler *s, SchedSlot &slot, uint64_t now)
{
    RtJob *job = s->jobs.push(static_cast<size_t>(&slot - s->slots),
                            slot.job_seq + 1, now, slot.task.D, slot.task.W);
    if (job == nullptr) return; // Reserva validada en sched_add_task.
    slot.job_seq++;
    slot.release_tick = now;
    emit(s, SchedEventType::Release, static_cast<uint32_t>(now),
         static_cast<uint16_t>(slot.task.id), static_cast<uint16_t>(slot.job_seq));
}

static void ignore_press(Scheduler *s, uint32_t k, uint16_t id, uint16_t reason,
                         bool with_event)
{
    s->sporadic_ignored++;
    if (with_event) {
        emit(s, SchedEventType::SporadicIgnored, k, id, reason);
    }
}

bool sched_init(Scheduler *s, Algorithm algorithm, uint32_t ut_per_tick,
                SchedEventSink sink, SchedWorkFn work, void *ctx)
{
    if (s == nullptr || ut_per_tick == 0) {
        return false;
    }
    if (algorithm != Algorithm::RMS && algorithm != Algorithm::EDF) {
        return false;
    }

    s->count = 0;
    s->jobs.clear();
    s->clock = RtClock{};
    s->next_tick = 0;
    s->running_id = 0;
    s->miss_count = 0;
    s->pending_presses = 0;
    s->sporadic_ignored = 0;
    s->algorithm = algorithm;
    s->ut_per_tick = ut_per_tick;
    s->sink = sink;
    s->work = work;
    s->ctx = ctx;
    return true;
}

bool sched_add_task(Scheduler *s, const Task *task, uint16_t *slot_out)
{
    if (s == nullptr || task == nullptr) {
        return false;
    }
    if (s->count >= MAX_TASKS) {
        return false;
    }
    if (task->id == 0 || task->id > SCHED_MAX_TASK_ID) {
        return false;
    }
    if (task->W == 0 || task->T == 0 || task->D == 0) {
        return false;
    }
    if (task->type != TaskType::Periodic && task->type != TaskType::Sporadic) {
        return false;
    }

    for (size_t i = 0; i < s->count; ++i) {
        if (s->slots[i].task.id == task->id) {
            return false;
        }
        if (task->type == TaskType::Sporadic &&
            s->slots[i].task.type == TaskType::Sporadic) {
            return false;
        }
    }

    uint64_t reserved = rt_job_reservation(task->T, task->D);
    for (size_t i = 0; i < s->count; ++i)
        reserved += rt_job_reservation(s->slots[i].task.T, s->slots[i].task.D);
    if (reserved > RT_MAX_JOBS) return false;

    SchedSlot &slot = s->slots[s->count];
    slot = SchedSlot{};
    slot.task = *task;
    slot.task.remaining = 0;
    slot.task.absolute_deadline = 0;
    slot.task.job_active = false;
    slot.task.next_release =
        (task->type == TaskType::Periodic) ? s->clock.peek(s->next_tick) : 0;

    // ceil(W / sn) sin riesgo de desbordamiento.
    slot.cost_ticks = task->W / s->ut_per_tick +
                      ((task->W % s->ut_per_tick) != 0 ? 1U : 0U);

    ++s->count;
    if (slot_out != nullptr) {
        *slot_out = static_cast<uint16_t>(s->count); // ranura 1..8
    }
    return true;
}

void sched_sporadic_press(Scheduler *s, uint32_t presses)
{
    if (s == nullptr) {
        return;
    }

    const uint32_t room = UINT32_MAX - s->pending_presses;
    s->pending_presses += (presses < room) ? presses : room;
}

void sched_tick(Scheduler *s)
{
    if (s == nullptr) {
        return;
    }

    const uint32_t k = s->next_tick;
    const uint64_t now = s->clock.observe(k);

    // 1. Vencer cada trabajo por separado, nunca por una nueva liberación.
    for (auto &job : s->jobs.jobs)
        if (job.active && job.deadline <= now) abort_job(s, job, k);

    // 2. Liberaciones periódicas.
    for (size_t i = 0; i < s->count; ++i) {
        SchedSlot &slot = s->slots[i];
        if (slot.task.type != TaskType::Periodic ||
            slot.task.next_release > now) {
            continue;
        }

        release_job(s, slot, now);

        slot.task.next_release += ((now - slot.task.next_release) / slot.task.T + 1) * slot.task.T;
    }

    // 3. Pulsaciones de la tarea esporádica.
    if (s->pending_presses > 0) {
        const uint32_t presses = s->pending_presses;
        s->pending_presses = 0;

        SchedSlot *sporadic = nullptr;
        for (size_t i = 0; i < s->count; ++i) {
            if (s->slots[i].task.type == TaskType::Sporadic) {
                sporadic = &s->slots[i];
                break;
            }
        }

        for (uint32_t p = 0; p < presses; ++p) {
            const bool with_event = p < SCHED_MAX_PRESS_EVENTS_PER_TICK;

            if (sporadic == nullptr) {
                ignore_press(s, k, 0, SCHED_IGNORED_NO_TASK, with_event);
                continue;
            }

            // Primera liberación siempre válida; luego, al menos T ticks
            // desde la última liberación aceptada.
            const bool allowed = (sporadic->job_seq == 0) ||
                                 (now - sporadic->release_tick >= sporadic->task.T);
            if (allowed) {
                release_job(s, *sporadic, now);
            } else {
                ignore_press(s, k, static_cast<uint16_t>(sporadic->task.id),
                             SCHED_IGNORED_MIN_INTERVAL, with_event);
            }
        }
    }

    // 4. Selección.
    SchedSlot *chosen = nullptr;
    for (size_t i = 0; i < s->count; ++i) {
        SchedSlot &slot = s->slots[i];
        RtJob *head = s->jobs.first(i);
        slot.task.job_active = head != nullptr;
        if (!head) { slot.task.remaining = 0; continue; }
        slot.task.remaining = head->remaining;
        slot.task.absolute_deadline = head->deadline;
        if (chosen == nullptr || higher_priority(s, slot, *chosen)) {
            chosen = &slot;
        }
    }

    // 5. Cambio de tarea.
    const uint16_t new_id =
        (chosen != nullptr) ? static_cast<uint16_t>(chosen->task.id) : 0;
    if (new_id != s->running_id) {
        emit(s, SchedEventType::Switch, k, new_id, s->running_id);
        s->running_id = new_id;
    }

    // 6. Un tramo de trabajo.
    if (chosen != nullptr) {
        RtJob *job = s->jobs.first(static_cast<size_t>(chosen - s->slots));
        uint32_t ut = job->remaining;
        if (ut > s->ut_per_tick) {
            ut = s->ut_per_tick;
        }

        if (s->work != nullptr) {
            s->work(ut, s->ctx);
        }

        chosen->task.remaining -= ut;
        job->remaining -= ut;
        if (job->remaining == 0) {
            job->active = false;
            RtJob *head = s->jobs.first(static_cast<size_t>(chosen - s->slots));
            chosen->task.job_active = head != nullptr;
            chosen->task.remaining = head ? head->remaining : 0;
            emit(s, SchedEventType::Complete, k + 1,
                 static_cast<uint16_t>(chosen->task.id),
                 static_cast<uint16_t>(job->sequence));
        }
    }

    s->next_tick = k + 1;
}