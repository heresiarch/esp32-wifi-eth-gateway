#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t led_init(int gpio_num, uint32_t period_ms);
void led_set_blink_color(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Set the global maximum brightness scale (0 to 100).
 *        Setting this to 10-15 is highly recommended for indoor/desk use.
 * 
 * @param max_brightness_percent Value from 0 (off) to 100 (blindly bright).
 */
void led_set_max_brightness(uint8_t max_brightness_percent);

#ifdef __cplusplus
}
#endif
