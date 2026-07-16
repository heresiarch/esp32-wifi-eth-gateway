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
#include "lwip/ip_addr.h" 

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
    
    // Stop the watchdog immediately
    if (eth_timeout_timer != NULL) {
        xTimerStop(eth_timeout_timer, 0);
        ESP_LOGI(TAG, "Ethernet watchdog timer stopped.");
    }

    // Force the network interface up and active
    if (s_netif) {
        // Bring the interface up logically in the IP stack
        esp_netif_action_connected(s_netif, NULL, 0, NULL); 
        
        // Start the DHCP Server
        esp_err_t err = esp_netif_dhcps_start(s_netif);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "DHCP Server successfully started on 192.168.4.1");
        } else if (err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            ESP_LOGI(TAG, "DHCP Server was already running.");
        } else {
            ESP_LOGE(TAG, "Failed to start DHCP Server: %s", esp_err_to_name(err));
        }
    }
    break;

    case ETHERNET_EVENT_DISCONNECTED:
    ESP_LOGW(TAG, "Ethernet Link Down");
    if (s_netif) {
        esp_netif_dhcps_stop(s_netif);
    }
    if (eth_timeout_timer) {
        xTimerReset(eth_timeout_timer, 0);
    }
    break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGW(TAG, "Ethernet Stopped");
        if (eth_timeout_timer) {
            xTimerStop(eth_timeout_timer, 0);
        }
        break;
    }
}


/*---------------------------------------------------------------
 * Ethernet Initialization (DHCP Server Configuration)
 *-------------------------------------------------------------*/
void ethernet_init(void)
{
    esp_err_t ret;

    // 1. Create the Watchdog/Timeout Timer
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

    /* 2. Initialize the Ethernet Hardware (W5500 SPI) */
    ret = ethernet_init_all(&s_eth_handles, &s_eth_port_cnt);
    if ((ret != ESP_OK) || (s_eth_port_cnt == 0)) {
        ESP_LOGE(TAG, "W5500 Hardware Init Failed: %s", esp_err_to_name(ret));
        return;
    }

    /* 3. Register Event Handlers */
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

    /* -------------------------------------------------------------
     * CUSTOM DHCP SERVER NETWORK INTERFACE CONFIG
     * ------------------------------------------------------------- */

    // Prepare our static IP block
    esp_netif_ip_info_t ip_info;
    memset(&ip_info, 0, sizeof(esp_netif_ip_info_t));
    ip_info.ip.addr      = ESP_IP4TOADDR(192, 168, 4, 1);       // ESP32 Static IP (Gateway)
    ip_info.gw.addr      = ESP_IP4TOADDR(192, 168, 4, 1);       // Gateway points to itself
    ip_info.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0);     // Subnet Mask

    // Grab default inherent configurations for Ethernet
    esp_netif_inherent_config_t inherent_cfg = ESP_NETIF_INHERENT_DEFAULT_ETH();
    
    // CRITICAL FIX: Declare this netif strictly as a DHCP Server.
    // We omit "ESP_NETIF_DHCP_CLIENT" entirely so the ESP32 never tries to behave like a client.
    inherent_cfg.flags = ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_GARP | ESP_NETIF_FLAG_EVENT_IP_MODIFIED;
    inherent_cfg.ip_info = &ip_info;
    inherent_cfg.if_key = "ETH_DHCPS";
    inherent_cfg.if_desc = "eth_dhcp_server";
    inherent_cfg.route_prio = 50;

    // Build the outer configuration block
    esp_netif_config_t cfg = {
        .base = &inherent_cfg,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH
    };

    // Create the customized network interface
    s_netif = esp_netif_new(&cfg);
    if (s_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create Ethernet netif");
        return;
    }

    // CRITICAL FIX: To prevent 0x5007, we must ensure both DHCP state machines are inactive 
    // before applying/modifying static IP settings.
    esp_netif_dhcpc_stop(s_netif);             // Safe to call even if client isn't active
    esp_netif_dhcps_stop(s_netif);             // Stop default server thread to allow IP changes

    // Apply the static IP structure to bind the interface safely
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_netif, &ip_info));
    ESP_LOGI(TAG, "Interface bound to static IP: 192.168.4.1");

    /* ------------------------------------------------------------- */

    /* 4. Attach the W5500 Ethernet Driver to our Custom Netif */
    ret = esp_netif_attach(
        s_netif,
        esp_eth_new_netif_glue(s_eth_handles[0]));

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to attach Ethernet netif: %s",
                 esp_err_to_name(ret));
        return;
    }

    /* 5. Fire up the Physical Link */
    ret = esp_eth_start(s_eth_handles[0]);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Ethernet: %s",
                 esp_err_to_name(ret));
        return;
    }
}