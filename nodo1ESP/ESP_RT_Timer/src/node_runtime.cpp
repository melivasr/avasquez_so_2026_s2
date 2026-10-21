#include "lib/node_runtime.h"

#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_timer.h"

#include "lib/calibration.h"
#include "lib/event_tx.h"
#include "lib/event_queue.h"

// ut.h no tiene extern "C" y no se puede modificar.
extern "C" void unidad_trabajo(void);

// ================================================================
// Hardware
// ================================================================

// Índice de ranura en binario: bit 0 en GPIO 18 (LSB) ... bit 3 en GPIO 22.
static constexpr gpio_num_t LED_SLOT_PINS[4] = {
    GPIO_NUM_18, GPIO_NUM_19, GPIO_NUM_21, GPIO_NUM_22};
static constexpr gpio_num_t LED_MISS_PIN = GPIO_NUM_23;

static constexpr UBaseType_t SCHED_TASK_PRIORITY = 10;
static constexpr uint32_t SCHED_TASK_STACK_BYTES = 4096;
static constexpr BaseType_t SCHED_TASK_CORE = 1;

static constexpr uint32_t TICK_US_MIN = 1000;
static constexpr uint32_t TICK_US_MAX = 1000000;

// Bits de notificación a la tarea calendarizadora.
static constexpr uint32_t NOTIFY_TICK = 1U << 0; // desde la ISR
static constexpr uint32_t NOTIFY_CMD = 1U << 1;  // desde la tarea UART

// ================================================================
// Estado
// ================================================================

enum class CmdType : uint8_t { None, Calibrate, Start, Stop };

// Una orden a la vez: la tarea UART la llena y espera a g_done.
struct Command {
    CmdType type;
    uint8_t status;
    uint32_t tick_us;     // Calibrate
    uint32_t sn_result;   // Calibrate
    Algorithm algorithm;  // Start
    uint16_t report_n;    // Start
    size_t task_count;    // Start
    Task tasks[MAX_TASKS];
};

static Command g_cmd;
static SemaphoreHandle_t g_done = nullptr;
static SemaphoreHandle_t g_api_mutex = nullptr;
static TaskHandle_t g_worker = nullptr;

static Scheduler g_sched;
static NodeStats g_stats;
static gptimer_handle_t g_timer = nullptr;

static volatile bool g_running = false;
static bool g_calibrated = false;
static uint32_t g_sn = 0;
static uint32_t g_tick_us = 0;
static uint16_t g_report_n = 0;

// Escritos por la ISR (el +1 evita ++ sobre volatile).
static volatile uint32_t g_isr_ticks = 0;
static volatile uint32_t g_isr_time_us = 0; // 32 bits bajos de esp_timer

// ================================================================
// LEDs
// ================================================================

static void show_slot(uint32_t slot)
{
    for (uint32_t bit = 0; bit < 4; ++bit) {
        gpio_set_level(LED_SLOT_PINS[bit], (slot >> bit) & 1U);
    }
}

static void leds_configure()
{
    uint64_t mask = 1ULL << LED_MISS_PIN;
    for (gpio_num_t pin : LED_SLOT_PINS) {
        mask |= 1ULL << pin;
    }

    gpio_config_t cfg = {};
    cfg.pin_bit_mask = mask;
    cfg.mode = GPIO_MODE_OUTPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&cfg));

    show_slot(0);
    gpio_set_level(LED_MISS_PIN, 0);
}

static void leds_fail()
{
    show_slot(0xF);
    gpio_set_level(LED_MISS_PIN, 1);
}

// Ranura 1..8 según la posición en la tabla; 0 = ocioso.
static uint32_t slot_for_id(uint16_t id)
{
    for (size_t i = 0; i < g_sched.count; ++i) {
        if (g_sched.slots[i].task.id == id) {
            return static_cast<uint32_t>(i + 1);
        }
    }
    return 0;
}

// ================================================================
// Callbacks del núcleo
// ================================================================

// Se llama dentro de sched_tick(), antes de ejecutar el tramo.
static void on_sched_event(const SchedEvent *event, void *ctx)
{
    (void)ctx;

    // Los valores de SchedEventType coinciden con los de la trama Event.
    event_tx_push(static_cast<uint8_t>(event->type), event->tick,
                  event->id, event->arg);

    if (event->type == SchedEventType::Switch) {
        show_slot(slot_for_id(event->id));
    } else if (event->type == SchedEventType::Miss) {
        gpio_set_level(LED_MISS_PIN, 1); // enclavado hasta el próximo Start
    }
}

static void run_work(uint32_t ut, void *ctx)
{
    (void)ctx;
    for (uint32_t i = 0; i < ut; ++i) {
        unidad_trabajo();
    }
}

// ================================================================
// ISR del temporizador: solo cuenta y avisa
// ================================================================

static bool IRAM_ATTR on_tick_alarm(
    gptimer_handle_t timer,
    const gptimer_alarm_event_data_t *edata,
    void *user_ctx)
{
    (void)timer;
    (void)edata;
    (void)user_ctx;

    BaseType_t woken = pdFALSE;
    g_isr_time_us = static_cast<uint32_t>(esp_timer_get_time());
    g_isr_ticks = g_isr_ticks + 1;
    xTaskNotifyFromISR(g_worker, NOTIFY_TICK, eSetBits, &woken);
    return woken == pdTRUE;
}

// ================================================================
// Órdenes (se ejecutan en la tarea calendarizadora)
// ================================================================

static uint8_t do_calibrate()
{
    if (g_running) {
        return NODE_BAD_STATE;
    }

    const uint32_t sn = calibration_measure(g_cmd.tick_us);
    if (sn == 0) {
        return NODE_REJECTED;
    }

    g_sn = sn;
    g_tick_us = g_cmd.tick_us;
    g_calibrated = true;
    g_cmd.sn_result = sn;
    return NODE_OK;
}

static void timer_destroy(gptimer_handle_t timer, bool enabled)
{
    if (enabled) {
        gptimer_disable(timer);
    }
    gptimer_del_timer(timer);
}

static uint8_t do_start()
{
    if (g_running || !g_calibrated) {
        return NODE_BAD_STATE;
    }

    if (!sched_init(&g_sched, g_cmd.algorithm, g_sn, on_sched_event,
                    run_work, nullptr)) {
        return NODE_REJECTED;
    }
    for (size_t i = 0; i < g_cmd.task_count; ++i) {
        if (!sched_add_task(&g_sched, &g_cmd.tasks[i], nullptr)) {
            return NODE_REJECTED;
        }
    }

    gptimer_handle_t timer = nullptr;
    gptimer_config_t timer_config = {};
    timer_config.clk_src = GPTIMER_CLK_SRC_DEFAULT;
    timer_config.direction = GPTIMER_COUNT_UP;
    timer_config.resolution_hz = 1000000; // 1 conteo = 1 us
    if (gptimer_new_timer(&timer_config, &timer) != ESP_OK) {
        return NODE_REJECTED;
    }

    gptimer_event_callbacks_t callbacks = {};
    callbacks.on_alarm = on_tick_alarm;

    gptimer_alarm_config_t alarm = {};
    alarm.reload_count = 0;
    alarm.alarm_count = g_tick_us;
    alarm.flags.auto_reload_on_alarm = true;

    if (gptimer_register_event_callbacks(timer, &callbacks, nullptr) != ESP_OK ||
        gptimer_set_alarm_action(timer, &alarm) != ESP_OK) {
        timer_destroy(timer, false);
        return NODE_REJECTED;
    }

    // Se habilita desde esta tarea (núcleo 1): la interrupción se asigna
    // al núcleo que la solicita.
    if (gptimer_enable(timer) != ESP_OK) {
        timer_destroy(timer, false);
        return NODE_REJECTED;
    }

    g_stats = NodeStats{};
    g_report_n = g_cmd.report_n;
    g_isr_ticks = 0;
    event_tx_reset();
    gpio_set_level(LED_MISS_PIN, 0);
    show_slot(0);

    if (gptimer_start(timer) != ESP_OK) {
        timer_destroy(timer, true);
        return NODE_REJECTED;
    }

    g_timer = timer;
    g_running = true;
    return NODE_OK;
}

static uint8_t do_stop()
{
    if (!g_running) {
        return NODE_BAD_STATE;
    }

    g_running = false;
    gptimer_stop(g_timer);
    timer_destroy(g_timer, true);
    g_timer = nullptr;
    show_slot(0);
    return NODE_OK;
}

static void handle_command()
{
    switch (g_cmd.type) {
    case CmdType::Calibrate:
        g_cmd.status = do_calibrate();
        break;

    case CmdType::Start:
        g_cmd.status = do_start();
        if (g_cmd.status == NODE_OK) {
            // Se libera a la tarea UART (envía el ACK) y se procesa el
            // tick cero de inmediato; las alarmas marcan los siguientes.
            xSemaphoreGive(g_done);
            sched_tick(&g_sched);
            event_tx_kick();
            return;
        }
        break;

    case CmdType::Stop:
        g_cmd.status = do_stop();
        break;

    default:
        g_cmd.status = NODE_REJECTED;
        break;
    }

    xSemaphoreGive(g_done);
}

static void handle_tick()
{
    const uint32_t start_us = static_cast<uint32_t>(esp_timer_get_time());
    const uint32_t isr_ticks = g_isr_ticks;

    // Notificación sobrante de un tick ya absorbido: ignorar.
    if (isr_ticks < g_sched.next_tick) {
        return;
    }

    const uint32_t latency = start_us - g_isr_time_us;
    if (latency > g_stats.max_wake_latency_us) {
        g_stats.max_wake_latency_us = latency;
    }

    // Ticks no atendidos: se cuentan y el tiempo lógico salta al actual
    // (sin procesar ticks atrasados en ráfaga).
    if (isr_ticks > g_sched.next_tick) {
        const uint32_t lost = isr_ticks - g_sched.next_tick;
        g_stats.ticks_lost += lost;
        g_sched.next_tick = isr_ticks;
        event_tx_push(static_cast<uint8_t>(NodeEventType::TickLost), isr_ticks, 0,
                      static_cast<uint16_t>(lost > 0xFFFFU ? 0xFFFFU : lost));
    }

    sched_tick(&g_sched);
    event_tx_kick();

    const uint32_t busy =
        static_cast<uint32_t>(esp_timer_get_time()) - start_us;
    if (busy > g_stats.max_busy_us) {
        g_stats.max_busy_us = busy;
    }
}

static void worker_task(void *arg)
{
    (void)arg;

    while (true) {
        uint32_t bits = 0;
        // Bloqueante: sin busy waiting ni delay. Limpia todos los bits.
        xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY);

        if ((bits & NOTIFY_CMD) != 0) {
            handle_command();
        }
        if ((bits & NOTIFY_TICK) != 0 && g_running) {
            handle_tick();
        }
    }
}

// ================================================================
// API (se llama desde la tarea UART)
// ================================================================

bool node_runtime_init(void)
{
    if (g_worker != nullptr) {
        return true;
    }

    leds_configure();

    if (!event_tx_init()) {
        leds_fail();
        return false;
    }

    g_done = xSemaphoreCreateBinary();
    g_api_mutex = xSemaphoreCreateMutex();
    if (g_done == nullptr || g_api_mutex == nullptr) {
        leds_fail();
        return false;
    }

    if (xTaskCreatePinnedToCore(worker_task, "sched",
                                SCHED_TASK_STACK_BYTES, nullptr,
                                SCHED_TASK_PRIORITY, &g_worker,
                                SCHED_TASK_CORE) != pdPASS) {
        g_worker = nullptr;
        leds_fail();
        return false;
    }

    return true;
}

// Con g_api_mutex tomado y g_cmd lleno.
static uint8_t submit()
{
    xTaskNotify(g_worker, NOTIFY_CMD, eSetBits);
    xSemaphoreTake(g_done, portMAX_DELAY);
    return g_cmd.status;
}

uint8_t node_calibrate(uint32_t tick_us, uint32_t *ut_per_tick_out)
{
    if (g_worker == nullptr || ut_per_tick_out == nullptr ||
        tick_us < TICK_US_MIN || tick_us > TICK_US_MAX) {
        return NODE_REJECTED;
    }

    xSemaphoreTake(g_api_mutex, portMAX_DELAY);
    g_cmd = Command{};
    g_cmd.type = CmdType::Calibrate;
    g_cmd.tick_us = tick_us;
    const uint8_t status = submit();
    if (status == NODE_OK) {
        *ut_per_tick_out = g_cmd.sn_result;
    }
    xSemaphoreGive(g_api_mutex);
    return status;
}

uint8_t node_start(Algorithm algorithm, uint16_t report_n,
                   const Task *tasks, size_t count)
{
    if (g_worker == nullptr || count > MAX_TASKS ||
        (count > 0 && tasks == nullptr)) {
        return NODE_REJECTED;
    }

    xSemaphoreTake(g_api_mutex, portMAX_DELAY);
    g_cmd = Command{};
    g_cmd.type = CmdType::Start;
    g_cmd.algorithm = algorithm;
    g_cmd.report_n = report_n;
    g_cmd.task_count = count;
    for (size_t i = 0; i < count; ++i) {
        g_cmd.tasks[i] = tasks[i];
    }
    const uint8_t status = submit();
    xSemaphoreGive(g_api_mutex);
    return status;
}

uint8_t node_stop(void)
{
    if (g_worker == nullptr) {
        return NODE_REJECTED;
    }

    xSemaphoreTake(g_api_mutex, portMAX_DELAY);
    g_cmd = Command{};
    g_cmd.type = CmdType::Stop;
    const uint8_t status = submit();
    if (status == NODE_OK) {
        // La ejecución ya terminó: se entregan los eventos pendientes antes
        // de que la tarea UART envíe el ACK de Stop.
        event_tx_flush();
    }
    xSemaphoreGive(g_api_mutex);
    return status;
}

bool node_is_running(void)
{
    return g_running;
}

NodeStats node_stats(void)
{
    NodeStats stats = g_stats;
    stats.events_dropped = event_tx_dropped();
    return stats;
}