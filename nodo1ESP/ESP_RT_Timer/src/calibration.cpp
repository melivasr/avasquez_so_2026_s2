#include "lib/calibration.h"

#include "driver/gptimer.h"
#include "esp_attr.h"
#include "esp_err.h"

// ut.h no tiene extern "C" y no se puede modificar.
extern "C" void unidad_trabajo(void);

// Cuántos ticks dura la medición. Más ticks dan un promedio más estable pero
// una calibración más larga (con tick de 10 ms son ~0,5 s).
static constexpr uint32_t CAL_WINDOW_TICKS = 50;
// Porcentaje del cómputo medido que se declara como disponible. El resto queda
// de margen para el calendarizador, el envío de eventos y la UART.
static constexpr uint32_t CAL_MARGIN_PERCENT = 80;

static volatile uint32_t cal_ticks = 0;

// La ISR solo cuenta ticks; todo lo demás pasa en el ciclo de medición.
static bool IRAM_ATTR on_cal_alarm(
    gptimer_handle_t timer,
    const gptimer_alarm_event_data_t *edata,
    void *user_ctx)
{
    (void)timer;
    (void)edata;
    (void)user_ctx;
    cal_ticks = cal_ticks + 1;
    return false;
}

// Mide cuántas unidades de trabajo (UT) caben en un tick, o devuelve 0 si algo
// falla. Corre unidad_trabajo() sin parar durante CAL_WINDOW_TICKS ticks de un
// temporizador de hardware y cuenta cuántas veces la ejecutó.
//
// Ese ciclo no es una espera para temporizar nada: es la medición misma (el
// reloj lo da el temporizador, el ciclo solo carga la CPU como lo hará una
// tarea real).
uint32_t calibration_measure(uint32_t tick_us)
{
    gptimer_handle_t timer = nullptr;
    gptimer_config_t cfg = {};
    cfg.clk_src = GPTIMER_CLK_SRC_DEFAULT;
    cfg.direction = GPTIMER_COUNT_UP;
    cfg.resolution_hz = 1000000; // 1 conteo = 1 us

    if (gptimer_new_timer(&cfg, &timer) != ESP_OK) {
        return 0;
    }

    gptimer_event_callbacks_t callbacks = {};
    callbacks.on_alarm = on_cal_alarm;

    gptimer_alarm_config_t alarm = {};
    alarm.reload_count = 0;
    alarm.alarm_count = tick_us;
    alarm.flags.auto_reload_on_alarm = true;

    uint32_t result = 0;

    if (gptimer_register_event_callbacks(timer, &callbacks, nullptr) == ESP_OK &&
        gptimer_set_alarm_action(timer, &alarm) == ESP_OK &&
        gptimer_enable(timer) == ESP_OK) {

        cal_ticks = 0;

        if (gptimer_start(timer) == ESP_OK) {
            uint64_t ut_count = 0;

            while (cal_ticks < CAL_WINDOW_TICKS) {
                unidad_trabajo();
                ++ut_count;
            }

            gptimer_stop(timer);

            // UT por tick = ut_count / ticks de la ventana, y de eso solo se
            // declara el porcentaje de margen. Se redondea hacia abajo.
            result = static_cast<uint32_t>(
                (ut_count * CAL_MARGIN_PERCENT) /
                (100ULL * CAL_WINDOW_TICKS));
        }

        gptimer_disable(timer);
    }

    gptimer_del_timer(timer);
    return result;
}