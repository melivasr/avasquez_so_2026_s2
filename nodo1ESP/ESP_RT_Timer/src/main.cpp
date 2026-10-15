#include <stdio.h>
#include <string.h>

#include "task_parser.h"
#include "protocol.h"

static bool test_deploy_round_trip()
{
    static Task original[MAX_TASKS] = {};
    size_t original_count = 0;

    if (!load_local_tasks(original, MAX_TASKS, &original_count)) {
        printf("ERROR: no se pudieron cargar las tareas originales.\n");
        return false;
    }

    // 1. Serializar las tareas en lineas de texto.
    static uint8_t payload[FRAME_MAX_PAYLOAD] = {};
    size_t payload_length = 0;

    for (size_t i = 0; i < original_count; ++i) {
        int written = snprintf(
            reinterpret_cast<char *>(payload) + payload_length,
            sizeof(payload) - payload_length,
            "%lu,%c,%lu,%lu,%lu,%lu\n",
            (unsigned long)original[i].id,
            static_cast<char>(original[i].type),
            (unsigned long)original[i].W,
            (unsigned long)original[i].T,
            (unsigned long)original[i].D,
            (unsigned long)original[i].criticality
        );

        if (written < 0 ||
            static_cast<size_t>(written) >= sizeof(payload) - payload_length) {
            printf("ERROR: carga util demasiado grande.\n");
            return false;
        }

        payload_length += static_cast<size_t>(written);
    }

    printf("Carga util generada: %u bytes\n",
           (unsigned)payload_length);

    // 2. Codificar una trama DEPLOY_TASKS.
    static uint8_t encoded[FRAME_MAX_SIZE] = {};
    size_t encoded_length = 0;

    if (!encode_frame(
            MessageType::DeployTasks,
            payload,
            static_cast<uint16_t>(payload_length),
            encoded,
            sizeof(encoded),
            &encoded_length)) {
        printf("ERROR: no se pudo codificar la trama.\n");
        return false;
    }

    printf("Trama DEPLOY_TASKS codificada: %u bytes\n",
           (unsigned)encoded_length);

    // 3. Decodificar y validar el CRC.
    static Frame decoded = {};

    if (!decode_frame(encoded, encoded_length, &decoded)) {
        printf("ERROR: se rechazo una trama valida.\n");
        return false;
    }

    if (decoded.type != MessageType::DeployTasks) {
        printf("ERROR: tipo de mensaje incorrecto.\n");
        return false;
    }

    printf("Prueba de protocolo: trama y CRC validos.\n");

    // 4. Analizar las tareas recuperadas.
    static Task recovered[MAX_TASKS] = {};
    size_t recovered_count = 0;
    size_t start = 0;

    for (size_t i = 0; i <= decoded.length; ++i) {
        if (i != decoded.length && decoded.payload[i] != '\n') {
            continue;
        }

        size_t line_length = i - start;

        // Ignorar lineas vacias, por ejemplo la ultima tras '\n'.
        if (line_length > 0) {
            char line[96];

            if (line_length >= sizeof(line)) {
                printf("ERROR: linea de tarea demasiado larga.\n");
                return false;
            }

            memcpy(line, decoded.payload + start, line_length);
            line[line_length] = '\0';

            if (recovered_count >= MAX_TASKS ||
                !parse_task_line(line, &recovered[recovered_count])) {
                printf("ERROR: tarea recibida invalida.\n");
                return false;
            }

            ++recovered_count;
        }

        start = i + 1;
    }

    if (recovered_count != original_count) {
        printf("ERROR: cantidad de tareas diferente.\n");
        return false;
    }

    for (size_t i = 0; i < recovered_count; ++i) {
        printf(
            "Recibida: id=%lu tipo=%c W=%lu T=%lu D=%lu criticidad=%lu\n",
            (unsigned long)recovered[i].id,
            static_cast<char>(recovered[i].type),
            (unsigned long)recovered[i].W,
            (unsigned long)recovered[i].T,
            (unsigned long)recovered[i].D,
            (unsigned long)recovered[i].criticality
        );
    }

    printf("Prueba de despliegue local: %u tareas recuperadas.\n",
           (unsigned)recovered_count);

    // 5. Alterar un byte de la carga util sin actualizar el CRC.
    if (decoded.length == 0) {
        printf("ERROR: carga util vacia inesperada.\n");
        return false;
    }

    encoded[FRAME_HEADER_SIZE] ^= 0x01;

    static Frame corrupted = {};

    if (decode_frame(encoded, encoded_length, &corrupted)) {
        printf("ERROR: se acepto una trama corrupta.\n");
        return false;
    }

    printf("Prueba de protocolo: trama corrupta rechazada.\n");

    return true;
}

extern "C" void app_main(void)
{
    printf("\n=== Pruebas del parser ===\n");

    Task tasks[MAX_TASKS] = {};
    size_t count = 0;

    if (!load_local_tasks(tasks, MAX_TASKS, &count)) {
        printf("ERROR: no se pudieron cargar las tareas locales.\n");
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
        printf("ERROR: se acepto un tipo invalido.\n");
        return;
    }

    if (!parse_task_line("6,,10,20,20,1", &invalid)) {
        printf("Prueba de validacion: campo vacio rechazado.\n");
    } else {
        printf("ERROR: se acepto un campo vacio.\n");
        return;
    }

    printf("\n=== Prueba del protocolo DEPLOY_TASKS ===\n");

    if (!test_deploy_round_trip()) {
        printf("RESULTADO: PRUEBAS FALLIDAS.\n");
        return;
    }

    printf("RESULTADO: TODAS LAS PRUEBAS PASARON.\n");
}