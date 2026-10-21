// PARSER DE TAREAS
// Lee las líneas "id,tipo,W,T,D,criticidad" que manda el host.

#include "task_parser.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

// Pasa un texto a número. Solo acepta dígitos.
static bool parse_number(const char *text, uint32_t *value){
    // campo vacío, como en "3,P,,20,20,2"
    if (text[0] == '\0') {
        return false;
    }

    // nada de signos, espacios ni letras
    for (int i = 0; text[i] != '\0'; i++) {
        if (!isdigit((unsigned char)text[i])) {
            return false;
        }
    }

    errno = 0;
    unsigned long number = strtoul(text, NULL, 10);

    // el número no cabe en 32 bits
    if (errno == ERANGE ||
        number > UINT32_MAX) {
        return false;
    }
    *value = number;
    return true;
}

bool parse_task_line(const char *line, size_t length, Task *task){
    if (length > 0 &&
        line[length - 1] == '\r') {
        length--;
    }

    // línea vacía o demasiado larga
    if (length == 0 ||
        length >= TASK_LINE_MAX) {
        return false;
    }

    // Copiamos la línea para poder cortarla
    char buffer[TASK_LINE_MAX];
    memcpy(buffer, line, length);
    buffer[length] = '\0';

    // Cada coma se cambia por '\0', así cada campo queda como un texto aparte
    char *fields[TASK_FIELDS];
    int total = 1;
    fields[0] = buffer;

    for (size_t i = 0; i < length; i++) {
        if (buffer[i] != ',') {
            continue;
        }

        // sobran campos
        if (total == TASK_FIELDS) {
            return false;
        }

        buffer[i] = '\0';
        fields[total] = &buffer[i + 1];
        total++;
    }

    // faltan campos
    if (total != TASK_FIELDS) {
        return false;
    }

    Task candidate;

    // el tipo es P (periódica) o S (esporádica)
    if (strcmp(fields[1], "P") == 0) {
        candidate.type = TaskType::Periodic;
    } else if (strcmp(fields[1], "S") == 0) {
        candidate.type = TaskType::Sporadic;
    } else {
        return false;
    }

    // el resto son números
    if (!parse_number(fields[0], &candidate.id) ||
        !parse_number(fields[2], &candidate.W) ||
        !parse_number(fields[3], &candidate.T) ||
        !parse_number(fields[4], &candidate.D) ||
        !parse_number(fields[5], &candidate.criticality)) {
        return false;
    }

    // id, W, T y D en 0 no tienen sentido, y el id tiene que caber
    // en 16 bits (así va en los eventos)
    if (candidate.id == 0 ||
        candidate.id > TASK_ID_MAX ||
        candidate.W == 0 ||
        candidate.T == 0 ||
        candidate.D == 0) {
        return false;
    }

    *task = candidate;
    return true;
}

bool parse_task_list(const char *text, size_t length, Task *tasks, size_t capacity, size_t *count){
    size_t total = 0;
    size_t sporadic = 0;
    size_t start = 0;

    while (start < length) {
        // buscar dónde termina esta línea
        size_t end = start;
        while (end < length && text[end] != '\n') {
            end++;
        }

        const char *line = &text[start];
        size_t line_length = end - start;
        start = end + 1;

        // saltar líneas vacías ("" o solo "\r")
        if (line_length == 0 ||
            (line_length == 1 && line[0] == '\r')) {
            continue;
        }

        // ya no caben más tareas
        if (total == capacity) {
            return false;
        }

        Task *task = &tasks[total];

        // una línea mala invalida todo el despliegue
        if (!parse_task_line(line, line_length, task)) {
            return false;
        }

        // no puede haber dos tareas con el mismo id
        for (size_t i = 0; i < total; i++) {
            if (tasks[i].id == task->id) {
                return false;
            }
        }

        // solo se permite una tarea esporádica por nodo
        if (task->type == TaskType::Sporadic) {
            sporadic++;
        }
        if (sporadic > 1) {
            return false;
        }

        total++;
    }

    // no vino ninguna tarea
    if (total == 0) {
        return false;
    }

    *count = total;
    return true;
}
