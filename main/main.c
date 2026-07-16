/*
 * Minimal WiFi + W5500 example
 *
 * menuconfig:
 *   Example Configuration
 *      WiFi SSID
 *      WiFi Password
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_eth.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"


#include "sdkconfig.h"

#include "ethernet.h"
#include "wifi.h"
#include "proxy.h"
#include "led.h"


static const char *TAG = "MAIN";

/*---------------------------------------------------------------
 * Main
 *-------------------------------------------------------------*/

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(
        esp_event_loop_create_default());

    wifi_init();

    ethernet_init();

    ESP_LOGI(TAG, "WiFi + Ethernet started.");
    
    // TODO: Make these configurable via menuconfig
    proxy_start(CONFIG_SERVER_LISTEN_PORT, CONFIG_PROXY_TARGET_ADDRESS, CONFIG_PROXY_TARGET_PORT);


    // Initialize non-blocking 30 ms timer on GPIO 21
    ESP_ERROR_CHECK(led_init(CONFIG_RGB_LED_GPIO, 100));
    uint32_t counter = 0;
    while (1) {
        
        if (++counter >= 60) {

        counter = 0;

        ESP_LOGI(TAG,
                 "Heap=%u  MinHeap=%u",
                 esp_get_free_heap_size(),
                 esp_get_minimum_free_heap_size());
        }

        led_set_max_brightness(15);
        led_set_blink_color(0, 255, 0); // green
        led_set_max_brightness(15);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}