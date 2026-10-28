// Parser de una línea de tarea: id,tipo,W,T,D,criticidad
// Ejemplo: 3,P,40,20,20,2
//
// Es estricto a propósito: exactamente 6 campos, una sola coma entre ellos,
// sin espacios ni signos (salvo la criticidad, que puede ser negativa).
// Cualquier cosa rara se rechaza en lugar de interpretarse.

#include "lib/task_parser.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

// Entero decimal sin signo de 32 bits. Se revisa dígito por dígito antes de
// strtoul porque este acepta espacios y signos al inicio.
static bool parse_uint(const char *text, uint32_t *out)
{
    if (text == nullptr || *text == '\0') {
        return false;
    }

    for (const char *p = text; *p != '\0'; ++p) {
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
    }

    errno = 0;
    char *end = nullptr;
    unsigned long value = strtoul(text, &end, 10);

    if (errno != 0 || *end != '\0' || value > UINT32_MAX) {
        return false;
    }

    *out = static_cast<uint32_t>(value);
    return true;
}

// Criticidad: entero con signo de 32 bits; solo se admite un '-' al inicio.
static bool parse_criticality(const char *text, int32_t *out)
{
    const char *digits = text;
    if (*digits == '-') ++digits;
    if (*digits == '\0') return false;
    for (const char *p = digits; *p; ++p) {
        if (!isdigit((unsigned char)*p)) return false;
    }
    errno = 0;
    const long long value = strtoll(text, nullptr, 10);
    if (errno != 0 || value < INT32_MIN || value > INT32_MAX) return false;
    *out = static_cast<int32_t>(value);
    return true;
}

// Divide 'buffer' (que se modifica) en exactamente 6 campos separados por ','.
// No se usa strtok porque este junta las comas seguidas y dejaría pasar
// campos vacíos. Aquí un campo vacío falla después en parse_uint o en la
// comparación del tipo, y una coma de más falla en esta función.
static bool split_fields(char *buffer, char *fields[6])
{
    char *p = buffer;
    for (int i = 0; i < 6; ++i) {
        fields[i] = p;
        char *comma = strchr(p, ',');
        if (i == 5) {
            return comma == nullptr; // el último campo no admite más comas
        }
        if (comma == nullptr) {
            return false;            // faltan campos
        }
        *comma = '\0';
        p = comma + 1;
    }
    return true;
}

// Convierte una línea en una Task. Solo escribe en 'out' si toda la línea es
// válida.
bool parse_task_line(const char *line, Task *out)
{
    if (line == nullptr || out == nullptr) {
        return false;
    }

    // 95 caracteres como máximo más el terminador.
    char buffer[96];

    size_t len = strlen(line);
    if (len == 0 || len >= sizeof(buffer)) {
        return false;
    }

    // Se trabaja sobre una copia porque split_fields escribe en el texto.
    memcpy(buffer, line, len + 1);

    char *fields[6];
    if (!split_fields(buffer, fields)) {
        return false;
    }

    Task candidate = {};

    // Orden de los campos: id, tipo, W, T, D, criticidad (el tipo se ve abajo).
    if (!parse_uint(fields[0], &candidate.id) ||
        !parse_uint(fields[2], &candidate.W) ||
        !parse_uint(fields[3], &candidate.T) ||
        !parse_uint(fields[4], &candidate.D) ||
        !parse_criticality(fields[5], &candidate.criticality)) {
        return false;
    }

    // Ninguno de estos puede ser cero (id 0 se reserva para "ocioso").
    if (candidate.id == 0 ||
        candidate.W == 0 ||
        candidate.T == 0 ||
        candidate.D == 0) {
        return false;
    }

    if (strcmp(fields[1], "P") == 0) {
        candidate.type = TaskType::Periodic;
    } else if (strcmp(fields[1], "S") == 0) {
        candidate.type = TaskType::Sporadic;
    } else {
        return false;
    }

    // Estado de ejecución inicial; el calendarizador lo completa.
    candidate.remaining = 0;
    candidate.next_release = 0;
    candidate.absolute_deadline = 0;
    candidate.job_active = false;

    *out = candidate;
    return true;
}