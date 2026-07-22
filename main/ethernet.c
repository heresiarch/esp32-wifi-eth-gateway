#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "esp_event.h"
#include "esp_eth.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "ethernet_init.h"
#include "ethernet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "sdkconfig.h"
#include "esp_rom_sys.h"
#include "esp_heap_caps.h"
#include "lwip/inet.h"

static const char *TAG = "ETH";

/* Event handler for IP_EVENT_ETH_GOT_IP */
static void got_ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;

    ESP_LOGI(TAG, "Ethernet Got IP Address");
    ESP_LOGI(TAG, "~~~~~~~~~~~");
    ESP_LOGI(TAG, "ETHIP:" IPSTR, IP2STR(&ip_info->ip));
    ESP_LOGI(TAG, "ETHMASK:" IPSTR, IP2STR(&ip_info->netmask));
    ESP_LOGI(TAG, "ETHGW:" IPSTR, IP2STR(&ip_info->gw));
    ESP_LOGI(TAG, "~~~~~~~~~~~");
}

static void start_dhcp_server_after_connection(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_netif_t *eth_netif = esp_netif_next_unsafe(NULL);
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;
    while (eth_netif != NULL) {
        esp_eth_handle_t eth_handle_for_current_netif = esp_netif_get_io_driver(eth_netif);
        if (memcmp(&eth_handle, &eth_handle_for_current_netif, sizeof(esp_eth_handle_t)) == 0) {
            esp_netif_dhcps_start(eth_netif);
            ESP_LOGI(TAG, "DHCP server started on %s\n", esp_netif_get_desc(eth_netif));
        }
        eth_netif = esp_netif_next_unsafe(eth_netif);
    }
}

void ethernet_init(void){
    // Initialize Ethernet driver
    uint8_t eth_port_cnt = 0;
    esp_eth_handle_t *eth_handles;
    ESP_ERROR_CHECK(ethernet_init_all(&eth_handles, &eth_port_cnt));
    // Initialize TCP/IP network interface
    ESP_ERROR_CHECK(esp_netif_init());

    char if_key_str[10];
    char if_desc_str[10];
    esp_netif_config_t cfg;
    esp_netif_inherent_config_t eth_netif_cfg;
    ESP_LOGI(TAG, "Example will act as DHCP server");
    esp_netif_ip_info_t *ip_infos;

    ip_infos = calloc(eth_port_cnt, sizeof(esp_netif_ip_info_t));

    eth_netif_cfg = (esp_netif_inherent_config_t) {
        .get_ip_event = IP_EVENT_ETH_GOT_IP,
        .lost_ip_event = 0,
        .flags = ESP_NETIF_DHCP_SERVER,
        .route_prio = 50
    };
    cfg = (esp_netif_config_t) {
        .base = &eth_netif_cfg,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH
    };

    for (uint8_t i = 0; i < eth_port_cnt; i++) {
        sprintf(if_key_str, "ETH_S%d", i);
        sprintf(if_desc_str, "eth%d", i);

        esp_netif_ip_info_t ip_info_i = {
            .ip = {.addr = ipaddr_addr(CONFIG_ETHERNET_IP)},
            .netmask = {.addr = ipaddr_addr(CONFIG_ETHERNET_NETMASK)},
            .gw = {.addr = ipaddr_addr(CONFIG_ETHERNET_GATEWAY)}
        };
        ip_infos[i] = ip_info_i;

        eth_netif_cfg.if_key = if_key_str;
        eth_netif_cfg.if_desc = if_desc_str;
        eth_netif_cfg.route_prio -= i * 5;
        eth_netif_cfg.ip_info = &(ip_infos[i]);
        esp_netif_t *eth_netif = esp_netif_new(&cfg);
        
        // Set DHCP lease time using public API options
        uint32_t lease_opt = 7200; // Lease time in seconds
ESP_ERROR_CHECK(esp_netif_dhcps_option(eth_netif, ESP_NETIF_OP_SET, ESP_NETIF_IP_ADDRESS_LEASE_TIME, &lease_opt, sizeof(lease_opt)));
        // Attach Ethernet driver to TCP/IP stack
        ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handles[i])));
    }
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_CONNECTED, start_dhcp_server_after_connection, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, got_ip_event_handler, NULL));
    ESP_LOGI(TAG, "--------");
    // Start Ethernet driver state machine
    for (uint8_t i = 0; i < eth_port_cnt; i++) {
        ESP_ERROR_CHECK(esp_eth_start(eth_handles[i]));
        ESP_LOGI(TAG, "Network Interface %d: " IPSTR, i, IP2STR(&ip_infos[i].ip));
    }
    ESP_LOGI(TAG, "--------");
}