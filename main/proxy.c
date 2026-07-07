#include "proxy.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"

static const char *TAG = "PROXY";

#define PROXY_STACK_SIZE 4096
#define PROXY_PRIORITY   5
#define BUFFER_SIZE      1460

typedef struct
{
    uint16_t listen_port;
    uint16_t remote_port;
    char remote_ip[32];
} proxy_config_t;

static void proxy_task(void *arg)
{
    proxy_config_t *cfg = (proxy_config_t *)arg;

    uint8_t buffer[BUFFER_SIZE];

    struct sockaddr_in listen_addr = {};
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(cfg->listen_port);
    listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (listen_sock < 0) {
        ESP_LOGE(TAG, "socket() failed");
        goto exit;
    }

    int opt = 1;
    setsockopt(listen_sock,
               SOL_SOCKET,
               SO_REUSEADDR,
               &opt,
               sizeof(opt));

    if (bind(listen_sock,
             (struct sockaddr *)&listen_addr,
             sizeof(listen_addr)) != 0) {

        ESP_LOGE(TAG, "bind() failed");
        goto cleanup_listener;
    }

    if (listen(listen_sock, 1) != 0) {
        ESP_LOGE(TAG, "listen() failed");
        goto cleanup_listener;
    }

    ESP_LOGI(TAG,
             "Listening on port %u",
             cfg->listen_port);

    while (true) {

        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client =
            accept(listen_sock,
                   (struct sockaddr *)&client_addr,
                   &addr_len);

        if (client < 0)
            continue;

        ESP_LOGI(TAG, "Client connected");

        struct sockaddr_in remote_addr = {};
        remote_addr.sin_family = AF_INET;
        remote_addr.sin_port = htons(cfg->remote_port);

        inet_pton(AF_INET,
                  cfg->remote_ip,
                  &remote_addr.sin_addr);

        int server =
            socket(AF_INET,
                   SOCK_STREAM,
                   IPPROTO_TCP);

        if (server < 0) {
            close(client);
            continue;
        }

        if (connect(server,
                    (struct sockaddr *)&remote_addr,
                    sizeof(remote_addr)) != 0) {

            ESP_LOGE(TAG,
                     "Cannot connect to %s:%u",
                     cfg->remote_ip,
                     cfg->remote_port);

            close(client);
            close(server);
            continue;
        }

        ESP_LOGI(TAG,
                 "Connected to server");

        while (true) {

            fd_set readfds;

            FD_ZERO(&readfds);
            FD_SET(client, &readfds);
            FD_SET(server, &readfds);

            int maxfd = client > server ? client : server;

            int ret =
                select(maxfd + 1,
                       &readfds,
                       NULL,
                       NULL,
                       NULL);

            if (ret <= 0)
                break;

            if (FD_ISSET(client, &readfds)) {

                int len =
                    recv(client,
                         buffer,
                         sizeof(buffer),
                         0);

                if (len <= 0)
                    break;

                if (send(server,
                         buffer,
                         len,
                         0) != len)
                    break;
            }

            if (FD_ISSET(server, &readfds)) {

                int len =
                    recv(server,
                         buffer,
                         sizeof(buffer),
                         0);

                if (len <= 0)
                    break;

                if (send(client,
                         buffer,
                         len,
                         0) != len)
                    break;
            }
        }

        ESP_LOGI(TAG, "Connection closed");

        shutdown(client, SHUT_RDWR);
        shutdown(server, SHUT_RDWR);

        close(client);
        close(server);
    }

cleanup_listener:
    close(listen_sock);

exit:
    free(cfg);
    vTaskDelete(NULL);
}

void proxy_start(uint16_t listen_port,
                 const char *remote_ip,
                 uint16_t remote_port)
{
    proxy_config_t *cfg =
        (proxy_config_t *)malloc(sizeof(proxy_config_t));

    if (!cfg)
        return;

    cfg->listen_port = listen_port;
    cfg->remote_port = remote_port;

    strncpy(cfg->remote_ip,
            remote_ip,
            sizeof(cfg->remote_ip) - 1);

    cfg->remote_ip[sizeof(cfg->remote_ip) - 1] = '\0';

    xTaskCreate(
        proxy_task,
        "proxy",
        PROXY_STACK_SIZE,
        cfg,
        PROXY_PRIORITY,
        NULL);
}