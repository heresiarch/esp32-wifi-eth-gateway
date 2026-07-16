#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "sdkconfig.h"
#include "wifi.h"
#include "string.h"

static const char *TAG = "WIFI";

#define WIFI_TIMEOUT_MS     30000

static TimerHandle_t wifi_timeout_timer = NULL;


/*---------------------------------------------------------------
 * Timer Callback
 *-------------------------------------------------------------*/
static void wifi_timeout_callback(TimerHandle_t xTimer)
{
    ESP_LOGE(TAG, "WiFi timeout");

    esp_restart();
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
            
            // Start the 30-second connection timer
            if (wifi_timeout_timer != NULL) {
                xTimerReset(wifi_timeout_timer, 0);
                ESP_LOGI(TAG, "30s Wi-Fi connection watchdog timer started.");
            }
            
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGW(TAG, "WiFi Disconnected");
            
            // Restart the timer so we restart if we can't reconnect within 30s
            if (wifi_timeout_timer != NULL) {
                xTimerReset(wifi_timeout_timer, 0);
                ESP_LOGW(TAG, "Connection lost. 30s Wi-Fi watchdog timer restarted.");
            }
            
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

        // Got IP successfully! Stop the countdown watchdog.
        if (wifi_timeout_timer != NULL) {
            if (xTimerStop(wifi_timeout_timer, 0) == pdPASS) {
                ESP_LOGI(TAG, "Wi-Fi timeout timer stopped successfully.");
            }
        }
    }
}

/*---------------------------------------------------------------
 * WiFi Initialization
 *-------------------------------------------------------------*/
void wifi_init(void)
{
    esp_err_t ret;

    // 1. Create the watchdog timer (one-shot, auto-reload set to pdFALSE)
    wifi_timeout_timer = xTimerCreate("wifi_watchdog",
                                      pdMS_TO_TICKS(WIFI_TIMEOUT_MS),
                                      pdFALSE,
                                      NULL,
                                      wifi_timeout_callback);
    
    if (wifi_timeout_timer == NULL) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi watchdog timer!");
        esp_restart();
        return;
    }

    // 2. Initialize the default STA netif interface
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (sta_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create default Wi-Fi Station Netif interface.");
        esp_restart();
        return;
    }

    // 3. Initialize Wi-Fi configurations safely
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Wi-Fi driver: %s", esp_err_to_name(ret));
        esp_restart();
        return;
    }

    // Prepare credentials config
    static wifi_config_t wifi_cfg;
    memset(&wifi_cfg, 0, sizeof(wifi_config_t));

    strcpy((char *)wifi_cfg.sta.ssid, CONFIG_WIFI_SSID);
    strcpy((char *)wifi_cfg.sta.password, CONFIG_WIFI_PASSWORD);
    wifi_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN; 

    // 4. Configure Station mode
    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set Wi-Fi mode: %s", esp_err_to_name(ret));
        esp_restart();
        return;
    }

    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write Wi-Fi configuration: %s", esp_err_to_name(ret));
        esp_restart();
        return;
    }

    // 5. Register Event Handlers with checks
    ret = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WIFI events: %s", esp_err_to_name(ret));
        esp_restart();
        return;
    }

    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP events: %s", esp_err_to_name(ret));
        esp_restart();
        return;
    }

    // 6. Start internal RF Transceiver and Driver
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Wi-Fi driver: %s", esp_err_to_name(ret));
        esp_restart();
        return;
    }

    // 7. Apply optional power configurations safely
    ret = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Could not set power save state: %s", esp_err_to_name(ret));
    }

    ret = esp_wifi_set_max_tx_power(44);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Could not adjust TX Power: %s", esp_err_to_name(ret));
    }
}