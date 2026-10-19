#include "serial_receiver.h"

#include <string.h>

#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_err.h"

#include "protocol.h"
#include "task_parser.h"

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

static bool send_ack(MessageType original_type, uint8_t status)
{
    // ACK: tipo de mensaje original y estado.
    uint8_t payload[2] = {
        static_cast<uint8_t>(original_type),
        status
    };

    uint8_t encoded[FRAME_HEADER_SIZE + 2 + FRAME_CRC_SIZE];
    size_t encoded_length = 0;

    if (!encode_frame(
            MessageType::Ack,
            payload,
            sizeof(payload),
            encoded,
            sizeof(encoded),
            &encoded_length)) {
        return false;
    }

    int written = uart_write_bytes(
        SERIAL_PORT,
        reinterpret_cast<const char *>(encoded),
        encoded_length
    );

    return written == static_cast<int>(encoded_length);
}

static bool process_deploy(const Frame &frame)
{
    if (frame.type != MessageType::DeployTasks || frame.length == 0) {
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

            // No aceptar IDs duplicados en el mismo despliegue.
            for (size_t j = 0; j < count; ++j) {
                if (pending_tasks[j].id == pending_tasks[count].id) {
                    return false;
                }
            }

            ++count;
        }

        start = i + 1;
    }

    if (count == 0) {
        return false;
    }

    memcpy(deployed_tasks, pending_tasks, count * sizeof(Task));

    // La tabla queda preparada para el futuro calendarizador.
    // Aún no se ejecutan las tareas ni se inicia el tick cero.
    return send_ack(MessageType::DeployTasks, 0);
}

static void process_received_frame(const uint8_t *data, size_t length)
{
    if (!decode_frame(data, length, &decoded_frame)) {
        // Trama corrupta: se descarta. El reporte de errores
        // se añadirá posteriormente mediante la cola de eventos.
        return;
    }

    if (decoded_frame.type == MessageType::DeployTasks) {
        if (!process_deploy(decoded_frame)) {
            send_ack(MessageType::DeployTasks, 1);
        }
    } else {
        // Tipo válido a nivel de trama, pero aún no implementado.
        send_ack(decoded_frame.type, 2);
    }
}

void serial_receiver_run(void)
{
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
