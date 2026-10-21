#include "lib/sched_core.h"

// Orden dentro de cada tick (k = s->next_tick):
//   1. Plazos: trabajo activo con plazo absoluto <= k -> MISS y aborto.
//   2. Liberaciones periódicas con next_release <= k (si el trabajo anterior
//      sigue activo, caso D > T, se aborta como MISS antes de liberar).
//   3. Selección según RMS o EDF.
//   4. SWITCH si cambia la tarea en ejecución.
//   5. Ejecución de un tramo: min(sn, UT restantes). Si termina -> COMPLETE
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
    uint32_t key_a;
    uint32_t key_b;

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

static void abort_job(Scheduler *s, SchedSlot &slot, uint32_t tick)
{
    emit(s, SchedEventType::Miss, tick,
         static_cast<uint16_t>(slot.task.id),
         static_cast<uint16_t>(slot.job_seq & 0xFFFFU));
    s->miss_count++;
    slot.task.job_active = false;
    slot.task.remaining = 0;
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

    *s = Scheduler{};
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

    SchedSlot &slot = s->slots[s->count];
    slot = SchedSlot{};
    slot.task = *task;
    slot.task.remaining = 0;
    slot.task.absolute_deadline = 0;
    slot.task.job_active = false;
    slot.task.next_release =
        (task->type == TaskType::Periodic) ? s->next_tick : 0;

    // ceil(W / sn) sin riesgo de desbordamiento.
    slot.cost_ticks = task->W / s->ut_per_tick +
                      ((task->W % s->ut_per_tick) != 0 ? 1U : 0U);

    ++s->count;
    if (slot_out != nullptr) {
        *slot_out = static_cast<uint16_t>(s->count); // ranura 1..8
    }
    return true;
}

void sched_tick(Scheduler *s)
{
    if (s == nullptr) {
        return;
    }

    const uint32_t k = s->next_tick;

    // 1. Plazos vencidos.
    for (size_t i = 0; i < s->count; ++i) {
        SchedSlot &slot = s->slots[i];
        if (slot.task.job_active && slot.task.absolute_deadline <= k) {
            abort_job(s, slot, k);
        }
    }

    // 2. Liberaciones periódicas.
    for (size_t i = 0; i < s->count; ++i) {
        SchedSlot &slot = s->slots[i];
        if (slot.task.type != TaskType::Periodic ||
            slot.task.next_release > k) {
            continue;
        }

        if (slot.task.job_active) {
            abort_job(s, slot, k); // solo ocurre si D > T
        }

        slot.job_seq++;
        slot.release_tick = k;
        slot.task.job_active = true;
        slot.task.remaining = slot.task.W;
        slot.task.absolute_deadline = k + slot.task.D;

        do {
            slot.task.next_release += slot.task.T;
        } while (slot.task.next_release <= k);

        emit(s, SchedEventType::Release, k,
             static_cast<uint16_t>(slot.task.id),
             static_cast<uint16_t>(slot.job_seq & 0xFFFFU));
    }

    // 3. Selección.
    SchedSlot *chosen = nullptr;
    for (size_t i = 0; i < s->count; ++i) {
        SchedSlot &slot = s->slots[i];
        if (!slot.task.job_active) {
            continue;
        }
        if (chosen == nullptr || higher_priority(s, slot, *chosen)) {
            chosen = &slot;
        }
    }

    // 4. Cambio de tarea.
    const uint16_t new_id =
        (chosen != nullptr) ? static_cast<uint16_t>(chosen->task.id) : 0;
    if (new_id != s->running_id) {
        emit(s, SchedEventType::Switch, k, new_id, s->running_id);
        s->running_id = new_id;
    }

    // 5. Un tramo de trabajo.
    if (chosen != nullptr) {
        uint32_t ut = chosen->task.remaining;
        if (ut > s->ut_per_tick) {
            ut = s->ut_per_tick;
        }

        if (s->work != nullptr) {
            s->work(ut, s->ctx);
        }

        chosen->task.remaining -= ut;
        if (chosen->task.remaining == 0) {
            chosen->task.job_active = false;
            emit(s, SchedEventType::Complete, k + 1,
                 static_cast<uint16_t>(chosen->task.id),
                 static_cast<uint16_t>(chosen->job_seq & 0xFFFFU));
        }
    }

    s->next_tick = k + 1;
}