
#ifndef TASK_H
#define TASK_H

#include <stdint.h>

enum class TaskType : char {
    Periodic = 'P',
    Sporadic = 'S'
};

// Por ejemplo: 3,P,40,20,20,2
struct Task {
    uint32_t id;
    TaskType type;
    uint32_t W;           // Trabajo requerido en UT
    uint32_t T;           // Período o intervalo mínimo, en ticks
    uint32_t D;           // Plazo relativo, en ticks
    uint32_t criticality;

    // Estado de ejecución local.
    uint32_t remaining;
    uint32_t next_release;
    uint32_t absolute_deadline;

    bool job_active;
};

#endif
