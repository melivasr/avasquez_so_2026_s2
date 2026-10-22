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

static constexpr uart_port_t SERIAL_PORT = UART_NUM_0;

static constexpr int SERIAL_BAUD = 115200;
static constexpr int UART_RX_BUFFER_SIZE = 1024;
static constexpr uint32_t RX_INTERBYTE_TIMEOUT_MS = 100;
static constexpr int UART_TX_BUFFER_SIZE = 1024;
static constexpr size_t MAX_DEPLOYED_TASKS = MAX_TASKS;

// Estado persistente: evita arreglos grandes en la pila de main.
static uint8_t rx_frame[FRAME_MAX_SIZE];
static Frame decoded_frame;
static Task deployed_tasks[MAX_DEPLOYED_TASKS];
static Task pending_tasks[MAX_DEPLOYED_TASKS];
static size_t deployed_count = 0;

// Envío de tramas: un solo búfer protegido por mutex, para que el ACK
// (tarea UART) y los eventos (tarea de transmisión) no se mezclen.
static SemaphoreHandle_t tx_mutex = nullptr;
static uint8_t tx_frame[FRAME_MAX_SIZE];

// Tramas corruptas descartadas (se informa en TickReport).
static volatile uint32_t bad_frames = 0;

uint32_t serial_bad_frames(void)
{
    return bad_frames;
}

void serial_bad_frames_reset(void)
{
    bad_frames = 0;
}

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

static bool send_ack(MessageType original_type, uint8_t status)
{
    // ACK: tipo de mensaje original y estado.
    uint8_t payload[2] = {
        static_cast<uint8_t>(original_type),
        status
    };

    return serial_send_frame(MessageType::Ack, payload, sizeof(payload));
}

static bool process_deploy(const Frame &frame)
{
    if (frame.type != MessageType::DeployTasks) {
        return false;
    }

    // Un NUL embebido truncaría la línea sin error; se rechaza la carga completa.
    if (memchr(frame.payload, '\0', frame.length) != nullptr) {
        return false;
    }

    size_t count = 0;
    size_t start = 0;

    // Validar todas las tareas en un arreglo temporal.
    // Solo se reemplaza la tabla activa si todas son válidas.
    for (size_t i = 0; i <= frame.length; ++i) {
        if (i != frame.length && frame.payload[i] != '\n') {
            continue;
        }

        size_t line_length = i - start;

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

            // Los eventos usan IDs de 16 bits.
            if (pending_tasks[count].id > SCHED_MAX_TASK_ID) {
                return false;
            }

            // No aceptar IDs duplicados en el mismo despliegue ni
            // más de una tarea esporádica por nodo.
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

    if (count == 0 && frame.length != 0) {
        return false;
    }

    memcpy(deployed_tasks, pending_tasks, count * sizeof(Task));
    deployed_count = count;

    // Las tareas empiezan a ejecutarse con el mensaje Start.
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

    // La medición corre en el núcleo del calendarizador (bloquea ~50 ticks).
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
    // Carga: [algoritmo: 0 = RMS, 1 = EDF][N: uint16 LE, periodo de TickReport]
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
    // Carga vacía. El ACK 0 solo indica que la pulsación quedó registrada.
    if (frame.length != 0) {
        send_ack(MessageType::SporadicRelease, NODE_REJECTED);
        return;
    }

    send_ack(MessageType::SporadicRelease, node_sporadic_release());
}

// Límite de la línea: parse_task_line usa un búfer de 96 B (95 + NUL).
static constexpr size_t TASK_LINE_MAX = 95;

static void process_add_task(const Frame &frame)
{
    size_t length = frame.length;

    // Se tolera un '\n' final.
    if (length > 0 && frame.payload[length - 1] == '\n') {
        --length;
    }

    // Vacía, demasiado larga o con NUL embebido (strtok truncaría sin error).
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

    // En ejecución, primero el calendarizador; la tabla del hilo UART solo
    // cambia si la aceptó (así ambas tablas siguen iguales).
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

static void process_received_frame(const uint8_t *data, size_t length)
{
    if (!decode_frame(data, length, &decoded_frame)) {
        // Trama corrupta: se descarta y se cuenta (TickReport).
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
        // Tipo válido a nivel de trama, pero aún no implementado.
        send_ack(decoded_frame.type, 2);
    }
}

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

    // UART0 utiliza los pines predeterminados de la consola ESP-IDF.
    ESP_ERROR_CHECK(uart_set_pin(
        SERIAL_PORT,
        GPIO_NUM_1,            // TX por defecto de UART0
        GPIO_NUM_3,            // RX por defecto de UART0
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

    size_t used = 0;
    size_t expected = 0;

    while (true) {
        uint8_t byte = 0;

        // Sin trama en curso: espera indefinida. Con trama a medias:
        // espera acotada. Ambas son bloqueantes, sin busy waiting.
        TickType_t wait = (used == 0)
            ? portMAX_DELAY
            : pdMS_TO_TICKS(RX_INTERBYTE_TIMEOUT_MS);

        int received = uart_read_bytes(SERIAL_PORT, &byte, 1, wait);

        if (received != 1) {
            // Timeout con trama incompleta: descartarla y resincronizar.
            if (used > 0) {
                bad_frames = bad_frames + 1;
            }
            used = 0;
            expected = 0;
            continue;
        }

        if (used < 2) {
            rx_frame[used++] = byte;

            if (used == 2 &&
                (rx_frame[0] != FRAME_MAGIC_0 ||
                 rx_frame[1] != FRAME_MAGIC_1)) {
                rx_frame[0] = rx_frame[1];
                used = (rx_frame[0] == FRAME_MAGIC_0) ? 1 : 0;
            }

            continue;
        }

        if (used >= sizeof(rx_frame)) {
            bad_frames = bad_frames + 1;
            used = 0;
            expected = 0;
            continue;
        }

        rx_frame[used++] = byte;

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

        if (expected != 0 && used == expected) {
            process_received_frame(rx_frame, used);
            used = 0;
            expected = 0;
        }
    }
}
