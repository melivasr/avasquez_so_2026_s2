// Datos de una tarea tal como llegan del host.
#pragma once

#include <stdint.h>

// 8 tareas simultáneas
#define MAX_TASKS 8U

enum class TaskType : char {
    Periodic = 'P',
    Sporadic = 'S',
};

// Por ejemplo: 3,P,40,20,20,2
struct Task {
    uint32_t id;
    TaskType type;
    uint32_t W; // trabajo requerido en UT
    uint32_t T; // período (P) o intervalo mínimo (S), en ticks
    uint32_t D; // plazo relativo, en ticks
    uint32_t criticality; // mayor = más crítica
};
