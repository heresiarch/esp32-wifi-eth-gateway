
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "sdkconfig.h"


#include "wifi.h"
#include "proxy.h"
#include "led.h"


static const char *TAG = "WIFI";

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

void wifi_init(void)
{
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 1. Adding "static" here ensures the memory survives, doesn't blowout the stack,
    // and guarantees every untouched byte inside the struct is cleanly zeroed out.
    static wifi_config_t wifi_cfg;
    
    // 2. Explicitly clear it out just to be ultra-safe
    memset(&wifi_cfg, 0, sizeof(wifi_config_t));

    // 3. Assign your credentials
    strcpy((char *)wifi_cfg.sta.ssid, CONFIG_WIFI_SSID);
    strcpy((char *)wifi_cfg.sta.password, CONFIG_WIFI_PASSWORD);

    // Optional but highly recommended: let the ESP32 select the strongest channel automatically
    wifi_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN; 

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

    // Enable Wi-Fi power saving
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));

    // Reduce TX power to 11 dBm
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(44));
}