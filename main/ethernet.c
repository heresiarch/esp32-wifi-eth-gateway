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
#include "esp_rom_sys.h"
#include "esp_heap_caps.h"
esp_netif_ip_info_t ip_info; 

static const char *TAG = "ETH";

#define W5500_RESET_GPIO   CONFIG_ETHERNET_SPI_PHY_RST0_GPIO
#define ETH_TIMEOUT_MS     30000

static TimerHandle_t eth_timeout_timer = NULL;

// Globale Variablen, um die Treiber-Instanzen im laufenden Betrieb ansteuern zu können
static esp_eth_handle_t *s_eth_handles = NULL;
static uint8_t s_eth_port_cnt = 0;

static bool s_restart_pending = false;

static esp_netif_t *s_netif = NULL;


/*---------------------------------------------------------------
 * Lokale Prototypen
 *-------------------------------------------------------------*/
static void hw_reset_w5500_only(void);
//static void eth_restart_without_reboot(void);


/*---------------------------------------------------------------
 * Hardware Reset Logic (Physischer Reset des W5500)
 *-------------------------------------------------------------*/
static void hw_reset_w5500_only(void)
{
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << W5500_RESET_GPIO),
        .pull_down_en = 0,
        .pull_up_en = 0
    };
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "Pulling W5500 Reset LOW...");
    gpio_set_level(W5500_RESET_GPIO, 0);
    esp_rom_delay_us(10000); // 10ms im LOW-Zustand (W5500 benötigt laut Datenblatt min. 2µs)

    ESP_LOGI(TAG, "Setting W5500 Reset HIGH...");
    gpio_set_level(W5500_RESET_GPIO, 1);
    
    // Wir geben dem internen PLL des W5500 50ms Zeit zum Einschwingen,
    // nutzen aber vTaskDelay statt esp_rom_delay, wenn wir nicht im ISR-Kontext sind.
    vTaskDelay(pdMS_TO_TICKS(50)); 
}

static void eth_health_check(void)
{
    uint8_t mac[6];
    esp_err_t err;

    err = esp_eth_ioctl(s_eth_handles[0], ETH_CMD_G_MAC_ADDR, mac);

    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "MAC OK: %02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2],
                 mac[3], mac[4], mac[5]);
    } else {
        ESP_LOGE(TAG,
                 "Cannot read MAC: %s",
                 esp_err_to_name(err));
    }

    esp_netif_ip_info_t ip;

    if (esp_netif_get_ip_info(s_netif, &ip) == ESP_OK) {
        ESP_LOGI(TAG,
                 "Current IP: " IPSTR,
                 IP2STR(&ip.ip));
    }

    ESP_LOGI(TAG,
         "Heap: %u  Min: %u",
         esp_get_free_heap_size(),
         esp_get_minimum_free_heap_size());
}



/*---------------------------------------------------------------
 * Timer Callback
 *-------------------------------------------------------------*/
static void eth_timeout_callback(TimerHandle_t xTimer)
{
    ESP_LOGW(TAG, "Ethernet watchdog timeout.");
    eth_health_check();
    
    if (s_restart_pending) {
        return;
    }

    s_restart_pending = true;

    for (int i = 0; i < s_eth_port_cnt; i++) {
        if (s_eth_handles[i]) {
            ESP_LOGI(TAG, "Stopping Ethernet port %d...", i);
            esp_eth_stop(s_eth_handles[i]);
        }
    }
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

    if (eth_timeout_timer != NULL) {
        if (xTimerStop(eth_timeout_timer, 0) == pdPASS) {
            ESP_LOGI(TAG, "Ethernet watchdog timer stopped.");
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
        break;

    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Ethernet Link Up");
        break;

    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Ethernet Link Down");
       
        spi_bus_free(CONFIG_ETHERNET_SPI_HOST);
        hw_reset_w5500_only();
        abort();    
        if (eth_timeout_timer) {
             xTimerReset(eth_timeout_timer, 0);
        }
    break;

    case ETHERNET_EVENT_STOP:

    ESP_LOGW(TAG, "Ethernet Stopped");

    if (eth_timeout_timer) {
        xTimerStop(eth_timeout_timer, 0);
    }

    if (s_restart_pending) {

        ESP_LOGW(TAG, "Resetting W5500...");

        //hw_reset_w5500_only();

        vTaskDelay(pdMS_TO_TICKS(100));

        for (int i = 0; i < s_eth_port_cnt; i++) {

            esp_err_t err = esp_eth_start(s_eth_handles[i]);

            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_eth_start failed: %s",
                         esp_err_to_name(err));
            }
        }

        s_restart_pending = false;
    }
    break;
    }
}

/*---------------------------------------------------------------
 * Ethernet Initialization (Wird nur 1x beim Systemstart aufgerufen)
 *-------------------------------------------------------------*/
void ethernet_init(void)
{
    esp_err_t ret;

    eth_timeout_timer = xTimerCreate(
        "eth_watchdog",
        pdMS_TO_TICKS(ETH_TIMEOUT_MS),
        pdFALSE,
        NULL,
        eth_timeout_callback);

    if (eth_timeout_timer == NULL) {
        ESP_LOGE(TAG, "Failed to create watchdog timer!");
        return;
    }

    /* Initialize Ethernet hardware */
    ret = ethernet_init_all(&s_eth_handles, &s_eth_port_cnt);
    if ((ret != ESP_OK) || (s_eth_port_cnt == 0)) {
        ESP_LOGE(TAG, "W5500 Hardware Init Failed: %s", esp_err_to_name(ret));
        return;
    }

    /* Register event handlers */
    ESP_ERROR_CHECK(esp_event_handler_register(
        ETH_EVENT,
        ESP_EVENT_ANY_ID,
        eth_event_handler,
        NULL));

    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT,
        IP_EVENT_ETH_GOT_IP,
        eth_got_ip_handler,
        NULL));

    /* Create Ethernet netif */
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();

    s_netif = esp_netif_new(&cfg);
    if (s_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create Ethernet netif");
        return;
    }

    /* Attach Ethernet driver */
    ESP_ERROR_CHECK(esp_netif_attach(
        s_netif,
        esp_eth_new_netif_glue(s_eth_handles[0])));

    /* Stop DHCP client */
    ESP_ERROR_CHECK(esp_netif_dhcpc_stop(s_netif));

    

    esp_netif_ip_info_t ip_info;

    ESP_ERROR_CHECK(esp_netif_str_to_ip4(CONFIG_ETHERNET_IP, &ip_info.ip));
    ESP_ERROR_CHECK(esp_netif_str_to_ip4(CONFIG_ETHERNET_NETMASK, &ip_info.netmask));
    ESP_ERROR_CHECK(esp_netif_str_to_ip4(CONFIG_ETHERNET_GATEWAY, &ip_info.gw));

    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_netif, &ip_info));

    /* Optional DNS */
    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(10, 0, 0, 1);

    ESP_ERROR_CHECK(esp_netif_set_dns_info(
        s_netif,
        ESP_NETIF_DNS_MAIN,
        &dns));

    /* Start Ethernet */
    ESP_ERROR_CHECK(esp_eth_start(s_eth_handles[0]));

    ESP_LOGI(TAG, "Ethernet started with static IP 10.0.0.1");
}