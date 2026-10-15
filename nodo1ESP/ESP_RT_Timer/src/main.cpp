#include <stdio.h>
#include "task_parser.h"

extern "C" void app_main(void)
{
    Task tasks[MAX_TASKS] = {};
    size_t count = 0;

    if (!load_local_tasks(tasks, MAX_TASKS, &count)) {
        printf("Error: no se pudieron cargar las tareas locales.\n");
        return;
    }

    printf("Tareas cargadas: %u\n", (unsigned)count);

    for (size_t i = 0; i < count; ++i) {
        const Task &task = tasks[i];

        printf(
            "id=%lu tipo=%c W=%lu T=%lu D=%lu criticidad=%lu\n",
            (unsigned long)task.id,
            static_cast<char>(task.type),
            (unsigned long)task.W,
            (unsigned long)task.T,
            (unsigned long)task.D,
            (unsigned long)task.criticality
        );
    }

    Task invalid = {};

    if (!parse_task_line("6,X,10,20,20,1", &invalid)) {
        printf("Prueba de validacion: tipo invalido rechazado.\n");
    } else {
        printf("ERROR: se acepto una tarea con tipo invalido.\n");
    }
    if (!parse_task_line("6,,10,20,20,1", &invalid)) {
        printf("Prueba de validacion: campo vacio rechazado.\n");
    } else {
        printf("ERROR: se acepto una tarea con campo vacio.\n");
    }

}
