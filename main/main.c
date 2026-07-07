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

#include "ethernet_init.h"
#include "sdkconfig.h"
#include "proxy.h"


static const char *TAG = "NET";


/*---------------------------------------------------------------
 * Ethernet IP Event
 *-------------------------------------------------------------*/

static void eth_got_ip_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

    ESP_LOGI(TAG, "========== Ethernet ==========");
    ESP_LOGI(TAG, "IP      : " IPSTR, IP2STR(&event->ip_info.ip));
    ESP_LOGI(TAG, "Mask    : " IPSTR, IP2STR(&event->ip_info.netmask));
    ESP_LOGI(TAG, "Gateway : " IPSTR, IP2STR(&event->ip_info.gw));
}

/*---------------------------------------------------------------
 * Ethernet Events
 *-------------------------------------------------------------*/

static void eth_event_handler(void *arg,
                              esp_event_base_t event_base,
                              int32_t event_id,
                              void *event_data)
{
    switch (event_id) {

    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet Started");
        break;

    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Ethernet Link Up");
        break;

    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Ethernet Link Down");
        break;

    case ETHERNET_EVENT_STOP:
        ESP_LOGW(TAG, "Ethernet Stopped");
        break;
    }
}

/*---------------------------------------------------------------
 * WiFi Events
 *-------------------------------------------------------------*/

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT) {

        switch (event_id) {

        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "WiFi Started");
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGW(TAG, "WiFi Disconnected");
            esp_wifi_connect();
            break;

        default:
            break;
        }

    } else if (event_base == IP_EVENT &&
               event_id == IP_EVENT_STA_GOT_IP) {

        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

        ESP_LOGI(TAG, "============ WiFi ============");
        ESP_LOGI(TAG, "IP      : " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Mask    : " IPSTR, IP2STR(&event->ip_info.netmask));
        ESP_LOGI(TAG, "Gateway : " IPSTR, IP2STR(&event->ip_info.gw));
    }
}

/*---------------------------------------------------------------
 * WiFi
 *-------------------------------------------------------------*/

static void wifi_init(void)
{
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid = CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASSWORD,
        }
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            wifi_event_handler,
            NULL));

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            wifi_event_handler,
            NULL));

    ESP_ERROR_CHECK(esp_wifi_start());
}

/*---------------------------------------------------------------
 * Ethernet
 *-------------------------------------------------------------*/

static void ethernet_init(void)
{
    uint8_t eth_port_cnt = 0;
    esp_eth_handle_t *eth_handles = NULL;

    ESP_ERROR_CHECK(
        ethernet_init_all(
            &eth_handles,
            &eth_port_cnt));

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            ETH_EVENT,
            ESP_EVENT_ANY_ID,
            eth_event_handler,
            NULL));

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_ETH_GOT_IP,
            eth_got_ip_handler,
            NULL));

    for (int i = 0; i < eth_port_cnt; i++) {

        esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();

        esp_netif_t *netif = esp_netif_new(&cfg);

        ESP_ERROR_CHECK(
            esp_netif_attach(
                netif,
                esp_eth_new_netif_glue(
                    eth_handles[i])));

        ESP_ERROR_CHECK(
            esp_eth_start(
                eth_handles[i]));
    }
}

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
    
    proxy_start(8888, "192.168.178.5", 8888);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}