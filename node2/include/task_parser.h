// Convierte el texto de DEPLOY_TASKS en tareas.
// Formato de línea: id,tipo,W,T,D,criticidad
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "task.h"

#define TASK_FIELDS 6U // campos por línea
#define TASK_LINE_MAX 96U // largo máximo de una línea

// Lee una línea (no necesita terminar en '\0'; se ignora un '\r' final).
// Devuelve false si la línea está mal y entonces no toca task.
bool parse_task_line(const char *line, size_t length, Task *task);

// Lee todas las líneas separadas por '\n' (las vacías se saltan).
// Devuelve false si alguna línea está mal, hay IDs repetidos, más de una
// tarea esporádica, más tareas que capacity o ninguna tarea.
bool parse_task_list(const char *text, size_t length, Task *tasks, size_t capacity, size_t *count);
