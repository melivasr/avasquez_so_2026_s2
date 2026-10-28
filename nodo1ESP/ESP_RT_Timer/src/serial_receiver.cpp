#include "lib/serial_receiver.h"

#include <string.h>

#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "lib/protocol.h"
#include "lib/task_parser.h"
#include "lib/sched_core.h"
#include "lib/node_runtime.h"

// Lado serial del nodo: arma tramas hacia el host, recibe y valida las que
// llegan, y despacha cada mensaje según su tipo. El formato de las tramas
// está en protocol.cpp y en CONTRATO.md.

static constexpr uart_port_t SERIAL_PORT = UART_NUM_0;

static constexpr int SERIAL_BAUD = 115200;
static constexpr int UART_RX_BUFFER_SIZE = 1024;
// Si una trama queda a medias y no llega otro byte en este tiempo, se descarta
// y se vuelve a buscar la marca de inicio.
static constexpr uint32_t RX_INTERBYTE_TIMEOUT_MS = 100;
static constexpr int UART_TX_BUFFER_SIZE = 1024;
static constexpr size_t MAX_DEPLOYED_TASKS = MAX_TASKS;

// Todo esto es static para no cargar la pila de app_main con arreglos grandes.
static uint8_t rx_frame[FRAME_MAX_SIZE];
static Frame decoded_frame;
// Tabla de tareas desplegadas; es la que se le entrega al calendarizador en Start.
static Task deployed_tasks[MAX_DEPLOYED_TASKS];
// Borrador de DeployTasks: se llena y valida aquí, y solo se copia a la tabla
// real si todas las líneas son válidas (todo o nada).
static Task pending_tasks[MAX_DEPLOYED_TASKS];
static size_t deployed_count = 0;

// Un solo búfer de salida protegido por un mutex. Lo usan la tarea UART (ACKs
// y CalibrateResp) y la tarea de eventos; sin el mutex las tramas se mezclarían.
static SemaphoreHandle_t tx_mutex = nullptr;
static uint8_t tx_frame[FRAME_MAX_SIZE];

// Tramas recibidas que se descartaron (CRC, longitud o timeout). Se informa
// en el TickReport.
static volatile uint32_t bad_frames = 0;

uint32_t serial_bad_frames(void)
{
    return bad_frames;
}

void serial_bad_frames_reset(void)
{
    bad_frames = 0;
}

// Codifica y escribe una trama completa en una sola operación.
bool serial_send_frame(MessageType type, const uint8_t *payload, uint16_t length)
{
    if (tx_mutex == nullptr) {
        return false;
    }

    xSemaphoreTake(tx_mutex, portMAX_DELAY);

    size_t encoded_length = 0;
    bool ok = encode_frame(type, payload, length,
                           tx_frame, sizeof(tx_frame), &encoded_length);

    if (ok) {
        const int written = uart_write_bytes(
            SERIAL_PORT,
            reinterpret_cast<const char *>(tx_frame),
            encoded_length
        );
        ok = (written == static_cast<int>(encoded_length));
    }

    xSemaphoreGive(tx_mutex);
    return ok;
}

// ACK = [tipo del mensaje original][estado]. Ver tabla de estados en CONTRATO.md.
static bool send_ack(MessageType original_type, uint8_t status)
{
    uint8_t payload[2] = {
        static_cast<uint8_t>(original_type),
        status
    };

    return serial_send_frame(MessageType::Ack, payload, sizeof(payload));
}

// DeployTasks: texto con una tarea por línea. Devuelve true solo si ya mandó
// el ACK de éxito; si devuelve false, quien llama responde "rechazado".
static bool process_deploy(const Frame &frame)
{
    if (frame.type != MessageType::DeployTasks) {
        return false;
    }

    // Un byte NUL en medio cortaría la línea sin dar error; es más seguro
    // rechazar toda la carga.
    if (memchr(frame.payload, '\0', frame.length) != nullptr) {
        return false;
    }

    size_t count = 0;
    size_t start = 0;

    // Se recorre la carga separando por '\n' y se valida cada línea en
    // pending_tasks. La tabla real no se toca hasta el final.
    for (size_t i = 0; i <= frame.length; ++i) {
        if (i != frame.length && frame.payload[i] != '\n') {
            continue;
        }

        size_t line_length = i - start;

        // Las líneas vacías se saltan (por ejemplo, el '\n' del final).
        if (line_length > 0) {
            char line[96];

            if (line_length >= sizeof(line) ||
                count >= MAX_DEPLOYED_TASKS) {
                return false;
            }

            memcpy(line, frame.payload + start, line_length);
            line[line_length] = '\0';

            if (!parse_task_line(line, &pending_tasks[count])) {
                return false;
            }

            // Los eventos llevan el id en 16 bits.
            if (pending_tasks[count].id > SCHED_MAX_TASK_ID) {
                return false;
            }

            // No se permiten ids repetidos ni más de una esporádica por nodo.
            for (size_t j = 0; j < count; ++j) {
                if (pending_tasks[j].id == pending_tasks[count].id) {
                    return false;
                }
                if (pending_tasks[j].type == TaskType::Sporadic &&
                    pending_tasks[count].type == TaskType::Sporadic) {
                    return false;
                }
            }

            ++count;
        }

        start = i + 1;
    }

    // Una carga con contenido pero sin ninguna tarea válida es un error.
    // Una carga completamente vacía sí es válida y deja la tabla en blanco.
    if (count == 0 && frame.length != 0) {
        return false;
    }

    // Que las tareas quepan en la cola de trabajos del calendarizador.
    if (!rt_jobs_fit(pending_tasks, count)) return false;
    memcpy(deployed_tasks, pending_tasks, count * sizeof(Task));
    deployed_count = count;

    // Las tareas no empiezan a correr hasta recibir Start.
    return send_ack(MessageType::DeployTasks, 0);
}

static void process_calibrate(const Frame &frame)
{
    if (frame.length != 4) {
        send_ack(MessageType::CalibrateReq, 1);
        return;
    }

    const uint32_t tick_us =
        static_cast<uint32_t>(frame.payload[0]) |
        (static_cast<uint32_t>(frame.payload[1]) << 8) |
        (static_cast<uint32_t>(frame.payload[2]) << 16) |
        (static_cast<uint32_t>(frame.payload[3]) << 24);

    // Rango aceptado: 1 ms a 1 s.
    if (tick_us < 1000 || tick_us > 1000000) {
        send_ack(MessageType::CalibrateReq, 1);
        return;
    }

    // La medición corre en el hilo del calendarizador (bloquea ~50 ticks);
    // mientras tanto esta tarea espera la respuesta.
    uint32_t ut_por_tick = 0;
    const uint8_t status = node_calibrate(tick_us, &ut_por_tick);

    if (status != NODE_OK || ut_por_tick == 0) {
        send_ack(MessageType::CalibrateReq,
                 status != NODE_OK ? status : NODE_REJECTED);
        return;
    }

    uint8_t payload[4] = {
        static_cast<uint8_t>(ut_por_tick & 0xFF),
        static_cast<uint8_t>((ut_por_tick >> 8) & 0xFF),
        static_cast<uint8_t>((ut_por_tick >> 16) & 0xFF),
        static_cast<uint8_t>((ut_por_tick >> 24) & 0xFF)
    };

    serial_send_frame(MessageType::CalibrateResp, payload, sizeof(payload));
}

static void process_start(const Frame &frame)
{
    // Carga: [algoritmo: 0 = RMS, 1 = EDF][N: uint16 LE, período del TickReport]
    if (frame.length != 3 || frame.payload[0] > 1) {
        send_ack(MessageType::Start, NODE_REJECTED);
        return;
    }

    const Algorithm algorithm =
        (frame.payload[0] == 0) ? Algorithm::RMS : Algorithm::EDF;
    const uint16_t report_n =
        static_cast<uint16_t>(frame.payload[1]) |
        static_cast<uint16_t>(frame.payload[2] << 8);

    send_ack(MessageType::Start,
             node_start(algorithm, report_n, deployed_tasks, deployed_count));
}

static void process_stop(const Frame &frame)
{
    if (frame.length != 0) {
        send_ack(MessageType::Stop, NODE_REJECTED);
        return;
    }

    send_ack(MessageType::Stop, node_stop());
}

static void process_sporadic_release(const Frame &frame)
{
    // Carga vacía. El ACK 0 solo dice que la pulsación quedó registrada; si
    // libera o se ignora lo informan los eventos.
    if (frame.length != 0) {
        send_ack(MessageType::SporadicRelease, NODE_REJECTED);
        return;
    }

    send_ack(MessageType::SporadicRelease, node_sporadic_release());
}

// Largo máximo de una línea de tarea: parse_task_line usa un búfer de 96 B
// (95 caracteres + el terminador).
static constexpr size_t TASK_LINE_MAX = 95;

// AddTask (modo manual): agrega una sola tarea, también con el nodo corriendo.
static void process_add_task(const Frame &frame)
{
    size_t length = frame.length;

    // Se acepta un '\n' al final.
    if (length > 0 && frame.payload[length - 1] == '\n') {
        --length;
    }

    // Rechazar si está vacía, es demasiado larga o trae un NUL en medio (sin
    // esta revisión, el parser vería la línea truncada y no daría error).
    if (length == 0 || length > TASK_LINE_MAX ||
        memchr(frame.payload, '\0', length) != nullptr) {
        send_ack(MessageType::AddTask, NODE_REJECTED);
        return;
    }

    char line[TASK_LINE_MAX + 1];
    memcpy(line, frame.payload, length);
    line[length] = '\0';

    Task task = {};
    if (!parse_task_line(line, &task) ||
        task.id > SCHED_MAX_TASK_ID ||
        deployed_count >= MAX_DEPLOYED_TASKS) {
        send_ack(MessageType::AddTask, NODE_REJECTED);
        return;
    }

    for (size_t j = 0; j < deployed_count; ++j) {
        if (deployed_tasks[j].id == task.id ||
            (deployed_tasks[j].type == TaskType::Sporadic &&
             task.type == TaskType::Sporadic)) {
            send_ack(MessageType::AddTask, NODE_REJECTED);
            return;
        }
    }

    if (!rt_jobs_fit(deployed_tasks, deployed_count, &task)) {
        send_ack(MessageType::AddTask, NODE_REJECTED);
        return;
    }

    // Si el nodo está corriendo, primero se la damos al calendarizador. La
    // tabla de este hilo solo cambia si la aceptó, así ambas quedan iguales.
    if (node_is_running()) {
        uint16_t slot = 0;
        const uint8_t status = node_add_task(&task, &slot);
        if (status != NODE_OK) {
            send_ack(MessageType::AddTask, status);
            return;
        }
    }

    deployed_tasks[deployed_count++] = task;
    send_ack(MessageType::AddTask, NODE_OK);
}

// Recibe una trama completa (ya delimitada por el bucle de lectura), la
// valida con decode_frame y la reparte según su tipo.
static void process_received_frame(const uint8_t *data, size_t length)
{
    if (!decode_frame(data, length, &decoded_frame)) {
        // Trama corrupta: se descarta sin responder y se cuenta para el TickReport.
        bad_frames = bad_frames + 1;
        return;
    }

    if (decoded_frame.type == MessageType::DeployTasks) {
        if (node_is_running()) {
            send_ack(MessageType::DeployTasks, NODE_BAD_STATE);
        } else if (!process_deploy(decoded_frame)) {
            send_ack(MessageType::DeployTasks, 1);
        }
    } else if (decoded_frame.type == MessageType::CalibrateReq) {
        process_calibrate(decoded_frame);
    } else if (decoded_frame.type == MessageType::Start) {
        process_start(decoded_frame);
    } else if (decoded_frame.type == MessageType::Stop) {
        process_stop(decoded_frame);
    } else if (decoded_frame.type == MessageType::SporadicRelease) {
        process_sporadic_release(decoded_frame);
    } else if (decoded_frame.type == MessageType::AddTask) {
        process_add_task(decoded_frame);
    } else {
        // Trama válida pero de un tipo que el nodo no atiende (incluye los
        // que el nodo solo envía): se responde "no implementado".
        send_ack(decoded_frame.type, 2);
    }
}

// Bucle principal de app_main: lee la UART byte a byte y arma cada trama.
void serial_receiver_run(void)
{
    tx_mutex = xSemaphoreCreateMutex();
    if (tx_mutex == nullptr) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }

    const uart_config_t config = {
        .baud_rate = SERIAL_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT
    };

    ESP_ERROR_CHECK(uart_param_config(SERIAL_PORT, &config));

    // UART0 con los pines por defecto de la consola de ESP-IDF (los del
    // conversor USB-serial de la placa).
    ESP_ERROR_CHECK(uart_set_pin(
        SERIAL_PORT,
        GPIO_NUM_1,            // TX de UART0
        GPIO_NUM_3,            // RX de UART0
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE
    ));

    ESP_ERROR_CHECK(uart_driver_install(
        SERIAL_PORT,
        UART_RX_BUFFER_SIZE,
        UART_TX_BUFFER_SIZE,
        0,
        nullptr,
        0
    ));

    // 'used' = bytes de la trama acumulados en rx_frame.
    // 'expected' = largo total esperado; vale 0 mientras no se conoce (se
    // calcula al leer el encabezado).
    size_t used = 0;
    size_t expected = 0;

    while (true) {
        uint8_t byte = 0;

        // Sin trama en curso se espera sin límite. Con una trama a medias se
        // espera solo el timeout entre bytes. En ambos casos la tarea queda
        // bloqueada en el driver, no girando.
        TickType_t wait = (used == 0)
            ? portMAX_DELAY
            : pdMS_TO_TICKS(RX_INTERBYTE_TIMEOUT_MS);

        int received = uart_read_bytes(SERIAL_PORT, &byte, 1, wait);

        if (received != 1) {
            // Venció el timeout con la trama incompleta: se descarta y se
            // vuelve a buscar la marca de inicio.
            if (used > 0) {
                bad_frames = bad_frames + 1;
            }
            used = 0;
            expected = 0;
            continue;
        }

        // Primeros dos bytes: buscar la marca de inicio A5 5A.
        if (used < 2) {
            rx_frame[used++] = byte;

            // Si los dos primeros no son la marca, se corre una posición:
            // el segundo byte podría ser el inicio de una marca válida.
            if (used == 2 &&
                (rx_frame[0] != FRAME_MAGIC_0 ||
                 rx_frame[1] != FRAME_MAGIC_1)) {
                rx_frame[0] = rx_frame[1];
                used = (rx_frame[0] == FRAME_MAGIC_0) ? 1 : 0;
            }

            continue;
        }

        // Protección contra desborde de rx_frame (no debería ocurrir porque
        // la longitud se valida abajo).
        if (used >= sizeof(rx_frame)) {
            bad_frames = bad_frames + 1;
            used = 0;
            expected = 0;
            continue;
        }

        rx_frame[used++] = byte;

        // Ya se tiene el encabezado completo: se lee la longitud de la carga
        // para saber cuántos bytes faltan.
        if (used == FRAME_HEADER_SIZE) {
            uint16_t payload_length =
                static_cast<uint16_t>(rx_frame[3]) |
                (static_cast<uint16_t>(rx_frame[4]) << 8);

            if (payload_length > FRAME_MAX_PAYLOAD) {
                bad_frames = bad_frames + 1;
                used = 0;
                expected = 0;
                continue;
            }

            expected = FRAME_HEADER_SIZE +
                       payload_length +
                       FRAME_CRC_SIZE;
        }

        // Trama completa: se procesa (el CRC se verifica en decode_frame).
        if (expected != 0 && used == expected) {
            process_received_frame(rx_frame, used);
            used = 0;
            expected = 0;
        }
    }
}