#ifndef RT_JOBS_H
#define RT_JOBS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* RAM reservada al arrancar; no se usa malloc en el camino de tiempo real. */
#define RT_MAX_JOBS 64U

/* ceil(D/T), más una liberación para el jitter al saltar ticks perdidos.
 * Se comprueba al admitir/configurar, antes de aceptar nuevas tareas. */
static inline uint64_t rt_job_reservation(uint32_t period, uint32_t deadline) {
    return period ? (uint64_t)(deadline / period) + (deadline % period != 0) + 1U : UINT64_MAX;
}

#ifdef __cplusplus
extern "C++" {
template <class Task>
static bool rt_jobs_fit(const Task *tasks, size_t count, const Task *candidate = nullptr) {
    uint64_t reserved = candidate ? rt_job_reservation(candidate->T, candidate->D) : 0;
    for (size_t i = 0; i < count; ++i) reserved += rt_job_reservation(tasks[i].T, tasks[i].D);
    return reserved <= RT_MAX_JOBS;
}

struct RtJob {
    uint64_t release, deadline;
    uint32_t remaining, sequence;
    uint8_t slot;
    bool active;
};

struct RtClock {
    uint64_t now = 0;
    uint32_t last = 0;
    bool seen = false;

    uint64_t peek(uint32_t tick) const {
        return seen ? now + (uint32_t)(tick - last) : tick;
    }
    uint64_t observe(uint32_t tick) {
        now = peek(tick);
        last = tick;
        seen = true;
        return now;
    }
};

struct RtJobs {
    RtJob jobs[RT_MAX_JOBS] = {};

    void clear() { for (auto &job : jobs) job.active = false; }

    RtJob *push(size_t slot, uint32_t sequence, uint64_t release,
                uint32_t deadline, uint32_t work) {
        for (auto &job : jobs) if (!job.active) {
            job = {release, release + deadline, work, sequence, (uint8_t)slot, true};
            return &job;
        }
        return nullptr; // La reserva validada impide este caso en una tabla válida.
    }
    RtJob *first(size_t slot) {
        RtJob *head = nullptr;
        for (auto &job : jobs) if (job.active && job.slot == slot &&
                (!head || job.release < head->release)) head = &job;
        return head;
    }
};
}
#endif
#endif
