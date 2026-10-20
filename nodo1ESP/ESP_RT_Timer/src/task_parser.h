
#ifndef TASK_PARSER_H
#define TASK_PARSER_H

#include <stddef.h>
#include "task.h"

// Al menos 8 treas simultáneas
constexpr size_t MAX_TASKS = 8;

bool parse_task_line(const char *line, Task *out);
bool load_local_tasks(Task tasks[], size_t capacity, size_t *count);

#endif
