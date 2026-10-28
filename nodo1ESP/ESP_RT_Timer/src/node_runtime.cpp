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
#include "lib/serial_receiver.h"
#include "lib/tick_report.h"

// Pegamento entre el hardware del ESP32 y el núcleo del calendarizador.
//
// Hay tres hilos de FreeRTOS, solo como infraestructura (ver CONTRATO.md,
// sección "Uso de FreeRTOS"):
//   - sched (núcleo 1, prioridad 10): corre sched_tick() una vez por tick y
//     atiende las órdenes del host. Es el único que toca el calendarizador.
//   - app_main (núcleo 0): lee la UART y arma las respuestas.
//   - evtx (núcleo 0): transmite eventos (ver event_tx.cpp).
// El orden en que corren las tareas del usuario lo decide sched_core.cpp,
// no FreeRTOS. El tick sale de un temporizador de hardware, sin delays.

// ut.h no tiene extern "C" y no se puede modificar.
extern "C" void unidad_trabajo(void);

// ----------------------------------------------------------------
// Hardware
// ----------------------------------------------------------------

// LEDs de la tarea en ejecución: su ID en binario (0 = ocioso),
// bit 0 en GPIO 18 (menos significativo) hasta bit 3 en GPIO 22.
static constexpr gpio_num_t LED_SLOT_PINS[4] = {
    GPIO_NUM_18, GPIO_NUM_19, GPIO_NUM_21, GPIO_NUM_22};
// LED de plazo perdido.
static constexpr gpio_num_t LED_MISS_PIN = GPIO_NUM_23;

// Botón de la tarea esporádica: va a GND, con pull-up interno.
static constexpr gpio_num_t BUTTON_PIN = GPIO_NUM_27;
// Antirrebote: un flanco de bajada cuenta como pulsación solo si llegó al
// menos este tiempo después del flanco anterior (de subida o de bajada).
static constexpr uint32_t BUTTON_DEBOUNCE_US = 50000;

// El calendarizador va en el núcleo 1 y con la prioridad más alta para que
// ni la UART ni la transmisión de eventos lo retrasen.
static constexpr UBaseType_t SCHED_TASK_PRIORITY = 10;
static constexpr uint32_t SCHED_TASK_STACK_BYTES = 4096;
static constexpr BaseType_t SCHED_TASK_CORE = 1;

// Rango de tick que aceptamos: 1 ms a 1 s.
static constexpr uint32_t TICK_US_MIN = 1000;
static constexpr uint32_t TICK_US_MAX = 1000000;

#ifdef RT_FAULT_INJECT
// SOLO PARA LA PRUEBA DE TICK_LOST (entorno esp32dev_fault). El firmware que
// se entrega se compila sin esta bandera.
// Después de procesar el tick indicado se bloquea el calendarizador un rato,
// para que la ISR siga contando ticks que nadie atiende. Los tiempos están en
// centésimas de tick; el ",05" sobrante evita que la cantidad de ticks
// perdidos dependa del jitter y así el resultado es repetible.
#include "esp_rom_sys.h"
static constexpr uint32_t FAULT_STALL_1_TICK = 150;
static constexpr uint32_t FAULT_STALL_1_CENTI = 305;   // 3,05 ticks -> se pierden 2
static constexpr uint32_t FAULT_STALL_2_TICK = 580;
static constexpr uint32_t FAULT_STALL_2_CENTI = 4005;  // 40,05 ticks -> se pierden 39
#endif

// Bits con los que se despierta a la tarea calendarizadora.
static constexpr uint32_t NOTIFY_TICK = 1U << 0; // desde la ISR del temporizador
static constexpr uint32_t NOTIFY_CMD = 1U << 1;  // desde la tarea UART

// ----------------------------------------------------------------
// Estado
// ----------------------------------------------------------------

enum class CmdType : uint8_t { None, Calibrate, Start, Stop, AddTask };

// Orden que la tarea UART le pasa a la tarea calendarizadora. Hay una sola a
// la vez: la UART la llena, avisa y espera en g_done hasta que se atienda.
struct Command {
    CmdType type;
    uint8_t status;
    uint32_t tick_us;     // Calibrate
    uint32_t sn_result;   // Calibrate
    uint16_t slot_result; // AddTask
    Algorithm algorithm;  // Start
    uint16_t report_n;    // Start
    size_t task_count;    // Start (AddTask usa tasks[0] con task_count = 1)
    Task tasks[MAX_TASKS];
};

static Command g_cmd;
static SemaphoreHandle_t g_done = nullptr;
static SemaphoreHandle_t g_api_mutex = nullptr; // serializa las llamadas a la API
static TaskHandle_t g_worker = nullptr;

static Scheduler g_sched;
static NodeStats g_stats;
static gptimer_handle_t g_timer = nullptr;

static volatile bool g_running = false;
static bool g_calibrated = false;
static uint32_t g_sn = 0;       // UT por tick de la última calibración
static uint32_t g_tick_us = 0;  // tick con el que se calibró; es el de Start
static uint16_t g_report_n = 0;
static uint32_t g_next_report = 0;

// Los escribe la ISR del temporizador. Se hace "x = x + 1" porque ++ sobre
// una variable volatile está deprecado en C++20.
static volatile uint32_t g_isr_ticks = 0;
static volatile uint32_t g_isr_time_us = 0; // 32 bits bajos de esp_timer

// Pulsaciones de la esporádica. Cada contador tiene un solo escritor (la ISR
// del botón o la tarea UART), así no hace falta lock; la tarea calendarizadora
// solo los lee y recuerda hasta dónde ya procesó.
static volatile uint32_t g_button_presses = 0;
static volatile uint32_t g_host_presses = 0;
static volatile uint32_t g_last_edge_us = 0;
static uint32_t g_seen_button = 0; // solo la tarea calendarizadora
static uint32_t g_seen_host = 0;
static volatile bool g_has_sporadic = false;

// ----------------------------------------------------------------
// LEDs y botón
// ----------------------------------------------------------------

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

// Falla al inicializar: se encienden los cinco LEDs.
static void leds_fail()
{
    show_slot(0xF);
    gpio_set_level(LED_MISS_PIN, 1);
}

// ISR del botón. Solo anota la pulsación; el calendarizador hace el resto en
// el siguiente tick. El antirrebote se hace midiendo el tiempo entre flancos.
static void IRAM_ATTR on_button_edge(void *arg)
{
    (void)arg;

    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time());
    const uint32_t gap = now - g_last_edge_us;
    g_last_edge_us = now;

    // Solo cuenta el flanco de bajada (botón presionado) y con el tiempo
    // mínimo desde el flanco anterior, lo que también filtra el rebote al soltar.
    if (gpio_get_level(BUTTON_PIN) == 0 && gap >= BUTTON_DEBOUNCE_US) {
        g_button_presses = g_button_presses + 1;
    }
}

static bool button_init()
{
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << BUTTON_PIN;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_ANYEDGE;
    if (gpio_config(&cfg) != ESP_OK) {
        return false;
    }

    const esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { // ya instalado: está bien
        return false;
    }

    return gpio_isr_handler_add(BUTTON_PIN, on_button_edge, nullptr) == ESP_OK;
}

// Pasa al núcleo las pulsaciones nuevas desde el último tick (botón + host).
static void feed_presses()
{
    const uint32_t button = g_button_presses;
    const uint32_t host = g_host_presses;
    const uint32_t fresh = (button - g_seen_button) + (host - g_seen_host);
    g_seen_button = button;
    g_seen_host = host;

    if (fresh > 0) {
        sched_sporadic_press(&g_sched, fresh);
    }
}

// ----------------------------------------------------------------
// Callbacks del núcleo
// ----------------------------------------------------------------

// Sched_tick() la llama por cada evento, antes de ejecutar el tramo del tick.
// Manda el evento al anillo y refleja SWITCH y MISS en los LEDs.
static void on_sched_event(const SchedEvent *event, void *ctx)
{
    (void)ctx;

    // Los valores de SchedEventType coinciden con los de la trama Event, así
    // que se pasa el tipo tal cual.
    event_tx_push(static_cast<uint8_t>(event->type), event->tick,
                  event->id, event->arg);

    if (event->type == SchedEventType::Switch) {
        show_slot(event->id); // con 4 LEDs se ven los 4 bits bajos del ID
    } else if (event->type == SchedEventType::Miss) {
        gpio_set_level(LED_MISS_PIN, 1); // se queda encendido hasta el próximo Start
    }
}

// Ejecuta 'ut' unidades de trabajo (el tramo de un tick).
static void run_work(uint32_t ut, void *ctx)
{
    (void)ctx;
    for (uint32_t i = 0; i < ut; ++i) {
        unidad_trabajo();
    }
}

// ----------------------------------------------------------------
// ISR del temporizador: solo cuenta y avisa
// ----------------------------------------------------------------

static bool IRAM_ATTR on_tick_alarm(
    gptimer_handle_t timer,
    const gptimer_alarm_event_data_t *edata,
    void *user_ctx)
{
    (void)timer;
    (void)edata;
    (void)user_ctx;

    BaseType_t woken = pdFALSE;
    // Se guarda cuándo disparó, para medir luego la latencia de despertar.
    g_isr_time_us = static_cast<uint32_t>(esp_timer_get_time());
    g_isr_ticks = g_isr_ticks + 1;
    xTaskNotifyFromISR(g_worker, NOTIFY_TICK, eSetBits, &woken);
    return woken == pdTRUE;
}

// ----------------------------------------------------------------
// Órdenes del host (corren en la tarea calendarizadora)
// ----------------------------------------------------------------

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

    // Se reconstruye el calendarizador desde cero con las tareas desplegadas.
    if (!sched_init(&g_sched, g_cmd.algorithm, g_sn, on_sched_event,
                    run_work, nullptr)) {
        return NODE_REJECTED;
    }
    for (size_t i = 0; i < g_cmd.task_count; ++i) {
        if (!sched_add_task(&g_sched, &g_cmd.tasks[i], nullptr)) {
            return NODE_REJECTED;
        }
    }

    // Temporizador de hardware a 1 MHz (1 conteo = 1 us) que dispara cada
    // tick y se recarga solo. Es la única base de tiempo del calendarizador.
    gptimer_handle_t timer = nullptr;
    gptimer_config_t timer_config = {};
    timer_config.clk_src = GPTIMER_CLK_SRC_DEFAULT;
    timer_config.direction = GPTIMER_COUNT_UP;
    timer_config.resolution_hz = 1000000;
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

    // Se habilita desde esta tarea (núcleo 1) a propósito: la interrupción
    // queda asignada al núcleo que la pide, y así el tick cae donde corre
    // el calendarizador.
    if (gptimer_enable(timer) != ESP_OK) {
        timer_destroy(timer, false);
        return NODE_REJECTED;
    }

    // Las pulsaciones anteriores al Start no cuentan.
    g_seen_button = g_button_presses;
    g_seen_host = g_host_presses;
    g_has_sporadic = false;
    for (size_t i = 0; i < g_cmd.task_count; ++i) {
        if (g_cmd.tasks[i].type == TaskType::Sporadic) {
            g_has_sporadic = true;
        }
    }

    // Se reinician contadores y búferes antes de arrancar el temporizador.
    g_stats = NodeStats{};
    g_report_n = g_cmd.report_n;
    g_next_report = g_cmd.report_n;
    serial_bad_frames_reset();
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
    gpio_set_level(LED_MISS_PIN, 0);
    return NODE_OK;
}

static uint8_t do_add_task()
{
    if (!g_running) {
        return NODE_BAD_STATE;
    }

    uint16_t slot = 0;
    if (!sched_add_task(&g_sched, &g_cmd.tasks[0], &slot)) {
        return NODE_REJECTED;
    }

    if (g_cmd.tasks[0].type == TaskType::Sporadic) {
        g_has_sporadic = true; // desde aquí SporadicRelease del host es válido
    }

    // Esta función corre entre dos ticks, así que next_tick es el próximo a
    // procesar y ahí se hará la primera liberación de una periódica nueva.
    event_tx_push(static_cast<uint8_t>(NodeEventType::TaskAdded),
                  g_sched.next_tick,
                  static_cast<uint16_t>(g_cmd.tasks[0].id), slot);
    event_tx_kick();

    g_cmd.slot_result = slot;
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
            // Primero se libera a la tarea UART (que manda el ACK) y luego se
            // procesa el tick cero, que es "ahora". Los siguientes ticks los
            // marca el temporizador. Por eso los eventos del tick cero pueden
            // salir antes que el ACK.
            xSemaphoreGive(g_done);
            feed_presses();
            sched_tick(&g_sched);
            event_tx_kick();
            return;
        }
        break;

    case CmdType::Stop:
        g_cmd.status = do_stop();
        break;

    case CmdType::AddTask:
        g_cmd.status = do_add_task();
        break;

    default:
        g_cmd.status = NODE_REJECTED;
        break;
    }

    xSemaphoreGive(g_done);
}

// Si toca, deja listo el TickReport del tick que se acaba de procesar.
// Solo lo prepara; lo envía la tarea de transmisión.
static void post_report_if_due(uint32_t tick)
{
    if (!tick_report_due(g_report_n, tick, &g_next_report)) {
        return;
    }

    TickReportData data = {};
    data.tick = tick;
    data.deadline_misses = g_sched.miss_count;
    data.sporadic_ignored = g_sched.sporadic_ignored;
    data.ticks_lost = g_stats.ticks_lost;
    data.events_dropped = event_tx_dropped();
    data.bad_frames = serial_bad_frames();
    data.max_busy_us = g_stats.max_busy_us;
    data.max_wake_latency_us = g_stats.max_wake_latency_us;

    uint8_t payload[TICK_REPORT_SIZE];
    const size_t length = tick_report_encode(data, payload, sizeof(payload));
    event_tx_post_report(payload, length);
}

// Procesa un tick: detecta ticks perdidos, corre el calendarizador y mide
// cuánto tardó (para el margen de calibración).
static void handle_tick()
{
    const uint32_t start_us = static_cast<uint32_t>(esp_timer_get_time());
    const uint32_t isr_ticks = g_isr_ticks;

    // Una notificación de un tick que ya se había absorbido: se ignora.
    if (static_cast<int32_t>(isr_ticks - g_sched.next_tick) < 0) {
        return;
    }

    // Latencia entre la alarma del temporizador y el inicio de este proceso.
    const uint32_t latency = start_us - g_isr_time_us;
    if (latency > g_stats.max_wake_latency_us) {
        g_stats.max_wake_latency_us = latency;
    }

    // Si la ISR contó más ticks de los que se procesaron, hubo ticks sin
    // atender. Se cuentan, se avisa con TICK_LOST y el tiempo lógico salta al
    // tick actual; no se procesan los atrasados en ráfaga.
    if (isr_ticks != g_sched.next_tick) {
        const uint32_t lost = isr_ticks - g_sched.next_tick;
        g_stats.ticks_lost += lost;
        g_sched.next_tick = isr_ticks;
        event_tx_push(static_cast<uint8_t>(NodeEventType::TickLost), isr_ticks, 0,
                      static_cast<uint16_t>(lost > 0xFFFFU ? 0xFFFFU : lost));
    }

    const uint32_t tick = g_sched.next_tick; // tick que se procesa ahora
    feed_presses();
    sched_tick(&g_sched);

    // Tiempo que tomó todo el proceso del tick, incluidas las UT.
    const uint32_t busy =
        static_cast<uint32_t>(esp_timer_get_time()) - start_us;
    if (busy > g_stats.max_busy_us) {
        g_stats.max_busy_us = busy;
    }

    post_report_if_due(tick);
    event_tx_kick();
    #ifdef RT_FAULT_INJECT
        // Espera activa a propósito, solo en esta prueba (no existe en producción).
        if (tick == FAULT_STALL_1_TICK || tick == FAULT_STALL_2_TICK) {
            const uint32_t centi = (tick == FAULT_STALL_1_TICK) ? FAULT_STALL_1_CENTI
                                                                : FAULT_STALL_2_CENTI;
            esp_rom_delay_us(static_cast<uint32_t>(
                static_cast<uint64_t>(g_tick_us) * centi / 100));
        }
    #endif
}

// Bucle de la tarea calendarizadora. Duerme hasta que llegue un tick o una
// orden; nunca espera con delays ni en un ciclo.
static void worker_task(void *arg)
{
    (void)arg;

    while (true) {
        uint32_t bits = 0;
        // Bloquea hasta que haya una notificación y limpia todos los bits.
        xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY);

        if ((bits & NOTIFY_CMD) != 0) {
            handle_command();
        }
        if ((bits & NOTIFY_TICK) != 0 && g_running) {
            handle_tick();
        }
    }
}

// ----------------------------------------------------------------
// API (la llama la tarea UART)
// ----------------------------------------------------------------

bool node_runtime_init(void)
{
    if (g_worker != nullptr) {
        return true;
    }

    leds_configure();

    if (!button_init()) {
        leds_fail();
        return false;
    }

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

// Envía g_cmd a la tarea calendarizadora y espera el resultado. Se llama con
// g_api_mutex tomado y g_cmd ya lleno.
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
        // La ejecución ya terminó. Se mandan los eventos que quedaban antes
        // de que la tarea UART envíe el ACK de Stop, para que después de ese
        // ACK no llegue nada más.
        event_tx_flush();
    }
    xSemaphoreGive(g_api_mutex);
    return status;
}

uint8_t node_add_task(const Task *task, uint16_t *slot_out)
{
    if (g_worker == nullptr || task == nullptr) {
        return NODE_REJECTED;
    }
    if (!g_running) {
        return NODE_BAD_STATE;
    }

    xSemaphoreTake(g_api_mutex, portMAX_DELAY);
    g_cmd = Command{};
    g_cmd.type = CmdType::AddTask;
    g_cmd.task_count = 1;
    g_cmd.tasks[0] = *task;
    const uint8_t status = submit();
    if (status == NODE_OK && slot_out != nullptr) {
        *slot_out = g_cmd.slot_result;
    }
    xSemaphoreGive(g_api_mutex);
    return status;
}

// Liberación de la esporádica pedida por el host. Solo suma una pulsación;
// el calendarizador decide en el siguiente tick si libera o ignora.
uint8_t node_sporadic_release(void)
{
    if (g_worker == nullptr) {
        return NODE_REJECTED;
    }
    if (!g_running) {
        return NODE_BAD_STATE;
    }
    if (!g_has_sporadic) {
        return NODE_REJECTED;
    }

    g_host_presses = g_host_presses + 1; // único escritor: la tarea UART
    return NODE_OK;
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