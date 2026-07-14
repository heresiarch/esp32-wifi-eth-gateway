#include "led.h"
#include <stdio.h>
#include "driver/gptimer.h"
#include "esp_log.h"
#include "led_strip.h"

static const char *TAG = "LED";

static led_strip_handle_t led_strip = NULL;
static TaskHandle_t led_task_handle = NULL;
static gptimer_handle_t gptimer = NULL;

static portMUX_TYPE color_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t target_r = 255; 
static uint8_t target_g = 0;
static uint8_t target_b = 0;
static uint8_t max_brightness = 15; // Default to 15% max power to save your eyes!

void led_set_blink_color(uint8_t r, uint8_t g, uint8_t b)
{
    taskENTER_CRITICAL(&color_mux);
    target_r = r;
    target_g = g;
    target_b = b;
    taskEXIT_CRITICAL(&color_mux);
}

void led_set_max_brightness(uint8_t max_brightness_percent)
{
    if (max_brightness_percent > 100) max_brightness_percent = 100;
    taskENTER_CRITICAL(&color_mux);
    max_brightness = max_brightness_percent;
    taskEXIT_CRITICAL(&color_mux);
}

static bool IRAM_ATTR timer_on_alarm_cb(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx)
{
    BaseType_t high_task_awoken = pdFALSE;
    TaskHandle_t task_to_notify = (TaskHandle_t)user_ctx;

    if (task_to_notify != NULL) {
        vTaskNotifyGiveFromISR(task_to_notify, &high_task_awoken);
    }
    return (high_task_awoken == pdTRUE); 
}

static void led_strip_task(void *pvParameters)
{
    static int16_t brightness = 0;
    static int8_t fade_direction = 5; 
    uint8_t r, g, b, max_b;

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        brightness += fade_direction;
        if (brightness >= 100 || brightness <= 0) {
            fade_direction = -fade_direction; 
        }

        taskENTER_CRITICAL(&color_mux);
        r = target_r;
        g = target_g;
        b = target_b;
        max_b = max_brightness;
        taskEXIT_CRITICAL(&color_mux);

        // First apply the fading factor (0.0 to 1.0)
        uint32_t intermediate_r = (r * brightness) / 100;
        uint32_t intermediate_g = (g * brightness) / 100;
        uint32_t intermediate_b = (b * brightness) / 100;

        // Then scale down down by our eye-safe global max limit
        uint8_t final_r = (intermediate_r * max_b) / 100;
        uint8_t final_g = (intermediate_g * max_b) / 100;
        uint8_t final_b = (intermediate_b * max_b) / 100;

        led_strip_set_pixel(led_strip, 0, final_r, final_g, final_b); 
        led_strip_refresh(led_strip);
    }
}

esp_err_t led_init(int gpio_num, uint32_t period_ms)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = gpio_num,
        .max_leds = 1,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_XTAL, 
        .resolution_hz = 5 * 1000 * 1000, 
        .flags.with_dma = false,
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip);
    if (err != ESP_OK) return err;
    led_strip_clear(led_strip);

    BaseType_t ret = xTaskCreate(led_strip_task, "led_strip_task", 3072, NULL, 2, &led_task_handle);
    if (ret != pdPASS) return ESP_FAIL;

    gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000, 
    };
    err = gptimer_new_timer(&timer_config, &gptimer);
    if (err != ESP_OK) return err;

    gptimer_event_callbacks_t cbs = {
        .on_alarm = timer_on_alarm_cb,
    };
    err = gptimer_register_event_callbacks(gptimer, &cbs, led_task_handle);
    if (err != ESP_OK) return err;

    gptimer_alarm_config_t alarm_config = {
        .alarm_count = period_ms * 1000, 
        .reload_count = 0,
        .flags.auto_reload_on_alarm = true,
    };
    err = gptimer_set_alarm_action(gptimer, &alarm_config);
    if (err != ESP_OK) return err;

    err = gptimer_enable(gptimer);
    if (err != ESP_OK) return err;
    
    err = gptimer_start(gptimer);
    if (err != ESP_OK) return err;

    return ESP_OK;
}