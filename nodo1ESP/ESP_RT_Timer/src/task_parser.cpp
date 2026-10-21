
#include "lib/task_parser.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

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

bool parse_task_line(const char *line, Task *out)
{
    if (line == nullptr || out == nullptr) {
        return false;
    }

    char buffer[96];

    size_t len = strlen(line);
    if (len == 0 || len >= sizeof(buffer)) {
        return false;
    }

    memcpy(buffer, line, len + 1);

    char *fields[6];
    char *saveptr = nullptr;
    char *token = strtok_r(buffer, ",", &saveptr);

    for (int i = 0; i < 6; ++i) {
        if (token == nullptr) {
            return false;
        }

        fields[i] = token;
        token = strtok_r(nullptr, ",", &saveptr);
    }

    // Rechazar campos adicionales.
    if (token != nullptr) {
        return false;
    }

    Task candidate = {};

    if (!parse_uint(fields[0], &candidate.id) ||
        !parse_uint(fields[2], &candidate.W) ||
        !parse_uint(fields[3], &candidate.T) ||
        !parse_uint(fields[4], &candidate.D) ||
        !parse_uint(fields[5], &candidate.criticality)) {
        return false;
    }

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

    candidate.remaining = 0;
    candidate.next_release = 0;
    candidate.absolute_deadline = 0;
    candidate.job_active = false;

    *out = candidate;
    return true;
}

bool load_local_tasks(Task tasks[], size_t capacity, size_t *count)
{
    if (tasks == nullptr || count == nullptr || capacity == 0) {
        return false;
    }

    static const char *lines[] = {
        "3,P,40,20,20,2",
        "4,P,10,10,8,1",
        "5,S,6,15,10,3"
    };

    *count = 0;

    for (const char *line : lines) {
        if (*count >= capacity) {
            return false;
        }

        Task candidate = {};

        if (!parse_task_line(line, &candidate)) {
            return false;
        }

        tasks[*count] = candidate;
        ++(*count);
    }

    return true;
}
