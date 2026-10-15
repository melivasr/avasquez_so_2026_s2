
#include <stdio.h>
#include <inttypes.h> // Para uso correcto de enteros

// ESPIDF 
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" void unidad_trabajo(void);

// ========================================
// CONSTANTES
// ========================================

static constexpr gpio_num_t LED_PIN = GPIO_NUM_19;
static constexpr uint32_t TICK_US = 10000; // 10 ms
static constexpr uint32_t UT_POR_CICLO = 100; // 10 ms
// volatile -> puede cambiar por un flujo externo (interrupciones)
static volatile uint32_t tick_count = 0;

// ========================================
// Función para atender interrupt
// ========================================

//IRAM_ATTR coloca la func en Instr. Mem apropiada
static bool IRAM_ATTR on_timer_alarm(
  gptimer_handle_t timer,
  const gptimer_alarm_event_data_t *edata,
  void *user_ctx)
{
  (void)timer;
  (void)edata;
  (void)user_ctx;

  tick_count++;
  return false;
}

// ========================================
// Flujo principal
// Tick dura 10ms
// ========================================

extern "C" void app_main(void)
{
  // Configuar pin en modo output. Chequea error
  ESP_ERROR_CHECK(gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT));

  // Definir temporizador y timer
  gptimer_handle_t timer = nullptr;
  gptimer_config_t timer_config = {};
  timer_config.clk_src = GPTIMER_CLK_SRC_DEFAULT; // clk default
  timer_config.direction = GPTIMER_COUNT_UP; // aumentar contador con el tiempo
  timer_config.resolution_hz = 1000000; // 1Mhz -> 1 conteo cada ms
  // Crear timer con el temporizador
  ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &timer));

  // Registrar función de interrupción
  gptimer_event_callbacks_t callbacks = {};
  callbacks.on_alarm = on_timer_alarm;
  ESP_ERROR_CHECK(gptimer_register_event_callbacks(timer, &callbacks, nullptr));

  // Configurar alarma periodica cada 10 ms
  gptimer_alarm_config_t alarm_config = {};
  alarm_config.reload_count = 0; // iniciar en 0
  alarm_config.alarm_count = TICK_US; // TICK_US es constante = 10ms
  alarm_config.flags.auto_reload_on_alarm = true; // repetir automáticamente
  ESP_ERROR_CHECK(gptimer_set_alarm_action(timer, &alarm_config));

  // Habilitar e iniciar el timer
  ESP_ERROR_CHECK(gptimer_enable(timer));
  ESP_ERROR_CHECK(gptimer_start(timer));
  printf("Temporizador iniciado: tick de %" PRIu32 " us\n", TICK_US);

  uint32_t last_tick = tick_count;
  uint64_t last_us = esp_timer_get_time(); // funcion de timer.h
  uint32_t ciclo = 0;
  bool led_state = false;

  // Bucle que lee contador, si no es el mismo, lo actualiza
  while (true) {
    // Simular trabajo usando ut.c
    for (uint32_t i = 0; i<UT_POR_CICLO; i++){ unidad_trabajo(); }

    // Esperar un seg antes del sig reporte
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Medir el tiempo y ticks actual
    int64_t now_us = esp_timer_get_time();
    uint32_t now_tick = tick_count;

    // Definir tiempo y ticks transcurrido
    int64_t elapsed_us = now_us - last_us;
    uint32_t ticks_elapsed = now_tick - last_tick;

    // Actualizar
    last_us = now_us;
    last_tick = now_tick;

    led_state = !led_state; // Actualizar LED
    ESP_ERROR_CHECK(gpio_set_level(LED_PIN, led_state ? 1:0));

    printf(
      "Ciclo %" PRIu32
      " | Tiempo: %" PRId64 " us"
      " | Ticks: %" PRIu32
      " | UT ejecutadas: %" PRIu32 "\n",
      ++ciclo,
      elapsed_us,
      ticks_elapsed,
      UT_POR_CICLO
    );
  }
}
