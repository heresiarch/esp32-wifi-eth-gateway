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
#include "esp_rom_sys.h" // Für präzise ROM-Verzögerungen im Timer-Kontext

static const char *TAG = "ETH";

#define W5500_RESET_GPIO   CONFIG_ETHERNET_SPI_PHY_RST0_GPIO
#define ETH_TIMEOUT_MS     30000

static TimerHandle_t eth_timeout_timer = NULL;

/*---------------------------------------------------------------
 * Hardware Reset Logic (Optimized for Timer Context)
 *-------------------------------------------------------------*/
static void hw_reset_w5500_and_restart(void)
{
    ESP_LOGE(TAG, "Ethernet error/timeout! Resetting W5500 and restarting ESP32-C6...");

    // 1. Reset-Pin als Output konfigurieren
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << W5500_RESET_GPIO),
        .pull_down_en = 0,
        .pull_up_en = 0
    };
    gpio_config(&io_conf);

    // 2. Hardware-Reset-Sequenz für W5500 ausführen
    // (Wir nutzen hier esp_rom_delay_us, um den FreeRTOS-Timer-Task nicht zu blockieren)
    ESP_LOGI(TAG, "Pulling W5500 Reset LOW...");
    gpio_set_level(W5500_RESET_GPIO, 0);
    esp_rom_delay_us(100000); // 100ms im LOW-Zustand halten

    ESP_LOGI(TAG, "Setting W5500 Reset HIGH...");
    gpio_set_level(W5500_RESET_GPIO, 1);
    esp_rom_delay_us(50000);  // 50ms warten, bis PLL und Quarz des W5500 stabil schwingen

    // 3. Erst jetzt, da der W5500 wieder "wach" und stabil ist, starten wir den ESP32-C6 neu
    ESP_LOGI(TAG, "Restarting System...");
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

    // Erfolgreich verbunden: Watchdog stoppen
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
            // Startet den Timer neu (resettet die 30 Sekunden)
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

    // 1. Erstelle den Watchdog-Timer
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

    // 2. Hardware initialisieren
    ret = ethernet_init_all(&eth_handles, &eth_port_cnt);
    if (ret != ESP_OK || eth_port_cnt == 0) {
        ESP_LOGE(TAG, "W5500 Hardware Init Failed! code: %s", esp_err_to_name(ret));
        hw_reset_w5500_and_restart();
        return;
    }

    // 3. Event-Handler registrieren
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

    // 4. Netif konfigurieren und starten
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