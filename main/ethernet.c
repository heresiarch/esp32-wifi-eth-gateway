#include "esp_event.h"
#include "esp_eth.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "ethernet_init.h"
#include "ethernet.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "sdkconfig.h"

static const char *TAG = "ETH";

// Define your W5500 physical reset pin (change this to your actual GPIO schematic mapping)
#define W5500_RESET_GPIO   CONFIG_ETHERNET_SPI_PHY_RST0_GPIO
#define ETH_TIMEOUT_MS     30000

static TimerHandle_t eth_timeout_timer = NULL;

/*---------------------------------------------------------------
 * Hardware Reset Logic
 *-------------------------------------------------------------*/
static void hw_reset_w5500_and_restart(void)
{
    ESP_LOGE(TAG, "Ethernet error or timeout detected! Hardware resetting W5500 and ESP32-C6...");

    // Configure the reset GPIO
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << W5500_RESET_GPIO),
        .pull_down_en = 0,
        .pull_up_en = 0
    };
    gpio_config(&io_conf);

    // Pull W5500 RESET low to physically reset the chip
    gpio_set_level(W5500_RESET_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(100)); // Hold reset low for 100ms
    gpio_set_level(W5500_RESET_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(50));  // Allow some time for the W5500 PLL to stabilize

    // Restart the ESP32-C6
    esp_restart();
}

/*---------------------------------------------------------------
 * Timer Callback
 *-------------------------------------------------------------*/
static void eth_timeout_callback(TimerHandle_t xTimer)
{
    hw_reset_w5500_and_restart();
}

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

    // Got IP! Stop the watchdog countdown timer.
    if (eth_timeout_timer != NULL) {
        if (xTimerStop(eth_timeout_timer, 0) == pdPASS) {
            ESP_LOGI(TAG, "Ethernet timeout timer stopped successfully.");
        }
    }
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
        if (eth_timeout_timer != NULL) {
            xTimerStart(eth_timeout_timer, 0);
            ESP_LOGI(TAG, "30s connection watchdog timer started.");
        }
        break;

    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Ethernet Link Up");
        break;

    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Ethernet Link Down");
        if (eth_timeout_timer != NULL) {
            xTimerStart(eth_timeout_timer, 0);
            ESP_LOGW(TAG, "Link lost. 30s connection watchdog timer restarted.");
        }
        break;

    case ETHERNET_EVENT_STOP:
        ESP_LOGW(TAG, "Ethernet Stopped");
        if (eth_timeout_timer != NULL) {
            xTimerStop(eth_timeout_timer, 0);
        }
        break;
    }
}


/*---------------------------------------------------------------
 * Ethernet Initialization
 *-------------------------------------------------------------*/
void ethernet_init(void)
{
    uint8_t eth_port_cnt = 0;
    esp_eth_handle_t *eth_handles = NULL;
    esp_err_t ret;

    // 1. Create the watchdog timer (one-shot, auto-reload set to pdFALSE)
    eth_timeout_timer = xTimerCreate("eth_watchdog",
                                     pdMS_TO_TICKS(ETH_TIMEOUT_MS),
                                     pdFALSE,
                                     NULL,
                                     eth_timeout_callback);
    
    if (eth_timeout_timer == NULL) {
        ESP_LOGE(TAG, "Failed to create Ethernet watchdog timer!");
        hw_reset_w5500_and_restart();
        return;
    }

    // 2. Safely check Ethernet peripheral hardware initialization
    ret = ethernet_init_all(&eth_handles, &eth_port_cnt);
    if (ret != ESP_OK || eth_port_cnt == 0) {
        ESP_LOGE(TAG, "W5500 Hardware Init Failed! code: %s", esp_err_to_name(ret));
        hw_reset_w5500_and_restart();
        return;
    }

    // 3. Register Event Handlers with safety checks
    ret = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register ETH events: %s", esp_err_to_name(ret));
        hw_reset_w5500_and_restart();
        return;
    }

    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, eth_got_ip_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP events: %s", esp_err_to_name(ret));
        hw_reset_w5500_and_restart();
        return;
    }

    // 4. Attach Netif interfaces and start drivers safely
    for (int i = 0; i < eth_port_cnt; i++) {

        esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
        esp_netif_t *netif = esp_netif_new(&cfg);
        if (netif == NULL) {
            ESP_LOGE(TAG, "Failed to create network interface.");
            hw_reset_w5500_and_restart();
            return;
        }

        ret = esp_netif_attach(netif, esp_eth_new_netif_glue(eth_handles[i]));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to attach network glue layer: %s", esp_err_to_name(ret));
            hw_reset_w5500_and_restart();
            return;
        }

        ret = esp_eth_start(eth_handles[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start Ethernet: %s", esp_err_to_name(ret));
            hw_reset_w5500_and_restart();
            return;
        }
    }
}