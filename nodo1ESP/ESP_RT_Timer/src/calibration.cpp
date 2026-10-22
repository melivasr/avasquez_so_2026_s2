#include "lib/calibration.h"

#include "driver/gptimer.h"
#include "esp_attr.h"
#include "esp_err.h"

// ut.h no tiene extern "C" y no se puede modificar.
extern "C" void unidad_trabajo(void);

static constexpr uint32_t CAL_WINDOW_TICKS = 50;
static constexpr uint32_t CAL_MARGIN_PERCENT = 80;

static volatile uint32_t cal_ticks = 0;

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

            result = static_cast<uint32_t>(
                (ut_count * CAL_MARGIN_PERCENT) /
                (100ULL * CAL_WINDOW_TICKS));
        }

        gptimer_disable(timer);
    }

    gptimer_del_timer(timer);
    return result;
}