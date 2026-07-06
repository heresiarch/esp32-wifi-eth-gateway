/*
 * Minimal W5500 Ethernet example
 * ESP-IDF 6.x + ethernet_init component
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_eth.h"
#include "esp_log.h"
#include "esp_netif.h"

#include "ethernet_init.h"

static const char *TAG = "ETH";

static void got_ip_event_handler(void *arg,
                                 esp_event_base_t event_base,
                                 int32_t event_id,
                                 void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

    ESP_LOGI(TAG, "================================");
    ESP_LOGI(TAG, "Ethernet Got IP");
    ESP_LOGI(TAG, "IP      : " IPSTR, IP2STR(&event->ip_info.ip));
    ESP_LOGI(TAG, "Mask    : " IPSTR, IP2STR(&event->ip_info.netmask));
    ESP_LOGI(TAG, "Gateway : " IPSTR, IP2STR(&event->ip_info.gw));
    ESP_LOGI(TAG, "================================");
}

void app_main(void)
{
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_ETH_GOT_IP,
            got_ip_event_handler,
            NULL));

    uint8_t eth_port_cnt = 0;
    esp_eth_handle_t *eth_handles = NULL;

    ESP_ERROR_CHECK(
        ethernet_init_all(
            &eth_handles,
            &eth_port_cnt));

    for (int i = 0; i < eth_port_cnt; i++) {

        esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
        esp_netif_t *eth_netif = esp_netif_new(&cfg);

        ESP_ERROR_CHECK(
            esp_netif_attach(
                eth_netif,
                esp_eth_new_netif_glue(
                    eth_handles[i])));

        ESP_ERROR_CHECK(
            esp_eth_start(
                eth_handles[i]));
    }

    ESP_LOGI(TAG, "Ethernet started.");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}