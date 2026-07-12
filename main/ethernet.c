#include "esp_event.h"
#include "esp_eth.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "ethernet_init.h"
#include "ethernet.h"

static const char *TAG = "ETH";

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
 * Ethernet
 *-------------------------------------------------------------*/

void ethernet_init(void)
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