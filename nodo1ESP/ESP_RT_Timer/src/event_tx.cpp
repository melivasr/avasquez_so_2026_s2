#include "lib/event_tx.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lib/event_queue.h"
#include "lib/protocol.h"
#include "lib/serial_receiver.h"

// Transmisión de eventos y TickReport en segundo plano.
//
// El calendarizador solo mete registros en el anillo (rápido, sin tocar la
// UART). Esta tarea es la que los saca y los manda por el puerto serial, así
// que escribir a 115200 baudios nunca retrasa un tick.

// Prioridad por debajo del calendarizador (10) y por encima de app_main (1),
// para que el búfer se vacíe apenas hay datos sin molestar al tick.
static constexpr UBaseType_t TX_TASK_PRIORITY = 2;
// El lote de eventos es static; en la pila solo va la copia del reporte.
static constexpr uint32_t TX_TASK_STACK_BYTES = 3072;
// Mismo núcleo que la UART; el calendarizador corre solo en el otro.
static constexpr BaseType_t TX_TASK_CORE = 0;

static EventRing g_ring;
// Spinlock compartido entre el núcleo 1 (calendarizador, que encola) y el
// núcleo 0 (esta tarea, que desencola). Las secciones críticas son cortas.
static portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t g_tx_task = nullptr;
static SemaphoreHandle_t g_flush_done = nullptr;
static volatile bool g_flush_requested = false;

// Solo se conserva el último TickReport: si sale uno nuevo antes de que el
// anterior se transmita, lo reemplaza (al host solo le sirve el más reciente).
static constexpr size_t REPORT_MAX = 32;
static uint8_t g_report[REPORT_MAX];
static size_t g_report_len = 0; // 0 = no hay reporte pendiente

// Manda el TickReport pendiente, si hay uno. Solo la llama tx_task.
static void send_pending_report()
{
    uint8_t report[REPORT_MAX];
    size_t report_len = 0;

    // Se copia dentro de la sección crítica y se envía afuera, para no
    // mantener el spinlock durante la escritura a la UART.
    portENTER_CRITICAL(&g_lock);
    report_len = g_report_len;
    if (report_len > 0) {
        memcpy(report, g_report, report_len);
        g_report_len = 0;
    }
    portEXIT_CRITICAL(&g_lock);

    if (report_len > 0) {
        serial_send_frame(MessageType::TickReport, report,
                          static_cast<uint16_t>(report_len));
    }
}

static void tx_task(void *arg)
{
    (void)arg;

    static uint8_t payload[1 + EVENT_MAX_PER_FRAME * EVENT_RECORD_SIZE];

    while (true) {
        // Duerme hasta que el calendarizador avise que hay algo por enviar.
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        while (true) {
            portENTER_CRITICAL(&g_lock);
            const size_t length =
                event_ring_pop_payload(&g_ring, payload, sizeof(payload));
            portEXIT_CRITICAL(&g_lock);

            if (length == 0) {
                break;
            }

            // Si la escritura falla ese lote se pierde; no se reintenta.
            serial_send_frame(MessageType::Event, payload,
                              static_cast<uint16_t>(length));

            // Si el anillo está saturado este bucle casi nunca termina, así
            // que el reporte se envía entre lotes para que no quede
            // esperando indefinidamente.
            send_pending_report();
        }

        // Puede haber un reporte aunque no haya eventos en cola.
        send_pending_report();

        // Stop pide un flush y espera a que se haya vaciado todo.
        if (g_flush_requested) {
            g_flush_requested = false;
            xSemaphoreGive(g_flush_done);
        }
    }
}

bool event_tx_init(void)
{
    if (g_tx_task != nullptr) {
        return true;
    }

    event_ring_init(&g_ring);

    g_flush_done = xSemaphoreCreateBinary();
    if (g_flush_done == nullptr) {
        return false;
    }

    if (xTaskCreatePinnedToCore(tx_task, "evtx", TX_TASK_STACK_BYTES, nullptr,
                                TX_TASK_PRIORITY, &g_tx_task,
                                TX_TASK_CORE) != pdPASS) {
        g_tx_task = nullptr;
        return false;
    }

    return true;
}

// Se llama en cada Start: descarta lo que haya quedado de la corrida anterior.
void event_tx_reset(void)
{
    portENTER_CRITICAL(&g_lock);
    event_ring_init(&g_ring);
    g_report_len = 0;
    portEXIT_CRITICAL(&g_lock);
}

void event_tx_post_report(const uint8_t *payload, size_t length)
{
    if (payload == nullptr || length == 0 || length > REPORT_MAX) {
        return;
    }

    portENTER_CRITICAL(&g_lock);
    memcpy(g_report, payload, length);
    g_report_len = length;
    portEXIT_CRITICAL(&g_lock);
}

void event_tx_push(uint8_t type, uint32_t tick, uint16_t id, uint16_t arg)
{
    const EventRecord record = { type, tick, id, arg };

    portENTER_CRITICAL(&g_lock);
    event_ring_push(&g_ring, record); // si está lleno, descarta y lo cuenta
    portEXIT_CRITICAL(&g_lock);
}

// Despierta a la tarea de transmisión, pero solo si hay algo que mandar.
void event_tx_kick(void)
{
    if (g_tx_task == nullptr) {
        return;
    }

    portENTER_CRITICAL(&g_lock);
    const size_t pending = event_ring_count(&g_ring) + g_report_len;
    portEXIT_CRITICAL(&g_lock);

    if (pending > 0) {
        xTaskNotifyGive(g_tx_task);
    }
}

// Bloquea hasta que se transmitió todo lo pendiente. Lo usa Stop para que el
// ACK salga después del último evento.
void event_tx_flush(void)
{
    if (g_tx_task == nullptr) {
        return;
    }

    g_flush_requested = true;
    xTaskNotifyGive(g_tx_task);
    xSemaphoreTake(g_flush_done, portMAX_DELAY);
}

uint32_t event_tx_dropped(void)
{
    portENTER_CRITICAL(&g_lock);
    const uint32_t dropped = g_ring.dropped;
    portEXIT_CRITICAL(&g_lock);
    return dropped;
}