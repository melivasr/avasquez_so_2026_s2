#include "lib/sched_core.h"

// Núcleo del calendarizador (RMS y EDF).
//
// Este archivo no usa FreeRTOS ni toca hardware: recibe un "tick" a la vez
// por sched_tick() y decide qué trabajo avanza. El trabajo real y el envío de
// eventos entran como callbacks (work y sink). Por eso se puede compilar y
// probar en un PC con g++ (ver sched_core_test.cpp).
//
// Qué pasa en cada tick (k = s->next_tick):
//   1. Se abortan los trabajos cuyo plazo absoluto ya llegó (MISS).
//   2. Se liberan las tareas periódicas que les toca.
//   3. Se procesan las pulsaciones de la esporádica.
//   4. Se elige el trabajo a correr según RMS o EDF.
//   5. Si cambió la tarea en ejecución, se emite SWITCH.
//   6. Se ejecuta un tramo de min(sn, UT restantes). Si el trabajo termina,
//      se emite COMPLETE con marca k+1 (el tramo ocupa [k, k+1)).

static void emit(Scheduler *s, SchedEventType type, uint32_t tick,
                 uint16_t id, uint16_t arg)
{
    if (s->sink == nullptr) {
        return;
    }
    const SchedEvent event = { type, tick, id, arg };
    s->sink(&event, s->ctx);
}

// Devuelve true si 'a' tiene mayor prioridad que 'b'.
//   RMS: gana el menor período T (prioridad fija).
//   EDF: gana el menor plazo absoluto del trabajo pendiente más antiguo.
// Desempate, igual en ambos algoritmos: mayor criticidad y, si siguen
// empatadas, menor id. Es determinista, así que una misma carga siempre
// produce el mismo Gantt.
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

// Aborta un trabajo que no terminó a tiempo: informa el MISS y lo desactiva.
// Los demás trabajos (incluidos otros de la misma tarea) siguen intactos.
static void abort_job(Scheduler *s, RtJob &job, uint32_t tick)
{
    emit(s, SchedEventType::Miss, tick,
         static_cast<uint16_t>(s->slots[job.slot].task.id),
         static_cast<uint16_t>(job.sequence));
    s->miss_count++;
    job.active = false;
}

// Encola un trabajo nuevo de la tarea. Si todavía hay trabajos anteriores sin
// terminar (caso D > T) se quedan en la cola en orden FIFO; cada uno guarda
// su propio plazo, su trabajo restante y su número de secuencia.
static void release_job(Scheduler *s, SchedSlot &slot, uint64_t now)
{
    RtJob *job = s->jobs.push(static_cast<size_t>(&slot - s->slots),
                            slot.job_seq + 1, now, slot.task.D, slot.task.W);
    // No debería pasar: sched_add_task ya reservó espacio para esta tarea.
    if (job == nullptr) return;
    slot.job_seq++;
    slot.release_tick = now;
    emit(s, SchedEventType::Release, static_cast<uint32_t>(now),
         static_cast<uint16_t>(slot.task.id), static_cast<uint16_t>(slot.job_seq));
}

// Cuenta una pulsación descartada. El evento se omite si ya se emitieron
// demasiados en este tick (el contador sí cuenta todas).
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

// Agrega una tarea a la tabla. Devuelve false si no cabe o es inválida.
// El host ya hizo la admisión (utilización); aquí solo se validan los límites
// propios del nodo. 'slot_out' recibe la ranura 1..8 que ocupa en los LEDs.
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

    // La cola de trabajos tiene tamaño fijo (RT_MAX_JOBS). Una tarea puede
    // tener a la vez hasta ceil(D/T)+1 trabajos pendientes, así que se suma
    // esa reserva para todas las tareas y se rechaza si no alcanza.
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
    // Una periódica se libera por primera vez en el próximo tick a procesar.
    // La esporádica no se libera sola.
    slot.task.next_release =
        (task->type == TaskType::Periodic) ? s->clock.peek(s->next_tick) : 0;

    // ceil(W / sn) escrito así para evitar el desbordamiento de W + sn - 1.
    slot.cost_ticks = task->W / s->ut_per_tick +
                      ((task->W % s->ut_per_tick) != 0 ? 1U : 0U);

    ++s->count;
    if (slot_out != nullptr) {
        *slot_out = static_cast<uint16_t>(s->count); // ranura 1..8
    }
    return true;
}

// Anota pulsaciones (botón o host) para procesarlas al inicio del próximo
// tick. Satura en lugar de dar la vuelta si el contador se llenara.
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
    // 'now' es el tiempo lógico en 64 bits; con él se calculan los plazos
    // absolutos sin preocuparse por que el contador de ticks dé la vuelta.
    const uint64_t now = s->clock.observe(k);

    // 1. Plazos. Cada trabajo se vence por su cuenta; una liberación nueva
    //    nunca aborta a la anterior.
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

        // Próxima liberación: el siguiente múltiplo de T después de 'now'.
        // Normalmente es +T; si se perdieron ticks, salta en vez de liberar
        // varias veces seguidas para "ponerse al día".
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

            // La primera pulsación siempre libera. Las siguientes solo si
            // pasaron al menos T ticks desde la última liberación aceptada
            // (T es el intervalo mínimo entre llegadas).
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

    // 4. Selección. Cada tarea compite con su trabajo pendiente más antiguo
    //    (FIFO dentro de la tarea). En EDF se usa el plazo de ese trabajo.
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

    // 5. Cambio de tarea (id 0 = ocioso).
    const uint16_t new_id =
        (chosen != nullptr) ? static_cast<uint16_t>(chosen->task.id) : 0;
    if (new_id != s->running_id) {
        emit(s, SchedEventType::Switch, k, new_id, s->running_id);
        s->running_id = new_id;
    }

    // 6. Un tramo de trabajo: a lo sumo sn unidades de trabajo por tick.
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
            // Si había otro trabajo en cola, pasa a ser el actual de la tarea.
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