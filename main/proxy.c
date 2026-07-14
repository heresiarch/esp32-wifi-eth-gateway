#include "proxy.h"
#include <string.h>
#include <fcntl.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#include "led.h"

static const char *TAG = "PROXY";

#define PROXY_STACK_SIZE 3072 // Reduced because buffer is on the heap now!
#define PROXY_PRIORITY   5
#define BUFFER_SIZE      1460

typedef struct
{
    uint16_t listen_port;
    uint16_t remote_port;
    char remote_ip[32];
} proxy_config_t;

// Helper to set non-blocking mode safely
static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags != -1) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

// Robust send loop to handle partial TCP writes
static int send_all(int fd, const uint8_t *buf, int len) {
    int total_sent = 0;
    while (total_sent < len) {
        int sent = send(fd, buf + total_sent, len - total_sent, 0);
        if (sent <= 0) {
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                vTaskDelay(pdMS_TO_TICKS(5)); // Yield briefly and try again
                continue;
            }
            return -1; // General error
        }
        total_sent += sent;
    }
    return total_sent;
}

static void proxy_task(void *arg)
{
    proxy_config_t *cfg = (proxy_config_t *)arg;

    // Fix 1: Allocate large buffer on heap to prevent Task Stack Overflow
    uint8_t *buffer = malloc(BUFFER_SIZE);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate memory for network buffer");
        goto exit;
    }

    struct sockaddr_in listen_addr = {};
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(cfg->listen_port);
    listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "socket() failed");
        goto free_buffer;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(listen_sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) != 0) {
        ESP_LOGE(TAG, "bind() failed");
        goto cleanup_listener;
    }

    if (listen(listen_sock, 1) != 0) {
        ESP_LOGE(TAG, "listen() failed");
        goto cleanup_listener;
    }

    ESP_LOGI(TAG, "Listening on port %u", cfg->listen_port);

    while (true) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client = accept(listen_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (client < 0) {
            vTaskDelay(pdMS_TO_TICKS(10)); // Yield to keep system watchdog happy
            continue;
        }

        ESP_LOGI(TAG, "Client connected");
        led_set_max_brightness(100);
        led_set_blink_color(0, 255, 255); // Cyan

        struct sockaddr_in remote_addr = {};
        remote_addr.sin_family = AF_INET;
        remote_addr.sin_port = htons(cfg->remote_port);
        inet_pton(AF_INET, cfg->remote_ip, &remote_addr.sin_addr);

        int server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (server < 0) {
            close(client);
            continue;
        }

        // Connect with standard blocking (or simple timeout config)
        if (connect(server, (struct sockaddr *)&remote_addr, sizeof(remote_addr)) != 0) {
            ESP_LOGE(TAG, "Cannot connect to %s:%u", cfg->remote_ip, cfg->remote_port);
            close(client);
            close(server);
            continue;
        }

        // Fix 3: Set connected sockets to non-blocking to prevent deadlock lockups
        set_nonblocking(client);
        set_nonblocking(server);

        ESP_LOGI(TAG, "Connected to server");

        while (true) {
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(client, &readfds);
            FD_SET(server, &readfds);

            int maxfd = client > server ? client : server;

            // Timeout of 10 seconds to allow the loop to yield/breathe if idle
            struct timeval timeout = { .tv_sec = 10, .tv_usec = 0 };
            int ret = select(maxfd + 1, &readfds, NULL, NULL, &timeout);

            if (ret < 0) {
                // Select error
                break;
            }
            if (ret == 0) {
                // Timeout (Keepalive/No activity) - loop again
                continue;
            }

            // Client has sent data
            if (FD_ISSET(client, &readfds)) {
                int len = recv(client, buffer, BUFFER_SIZE, 0);
                if (len <= 0) {
                    if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        // Data wasn't actually ready yet (spurious select wake)
                        continue;
                    }
                    break; // Actual socket close or error
                }

                // Fix 4: Loop partial writes safely instead of breaking on mismatch
                if (send_all(server, buffer, len) < 0) {
                    break;
                }
            }

            // Server has sent data
            if (FD_ISSET(server, &readfds)) {
                int len = recv(server, buffer, BUFFER_SIZE, 0);
                if (len <= 0) {
                    if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        continue;
                    }
                    break;
                }

                if (send_all(client, buffer, len) < 0) {
                    break;
                }
            }
        }

        ESP_LOGI(TAG, "Connection closed");
        led_set_max_brightness(100);
        led_set_blink_color(255, 0, 0); // Red

        // Clean shutdown
        shutdown(client, SHUT_RDWR);
        shutdown(server, SHUT_RDWR);
        close(client);
        close(server);
    }

cleanup_listener:
    close(listen_sock);

free_buffer:
    free(buffer);

exit:
    free(cfg);
    vTaskDelete(NULL);
}

void proxy_start(uint16_t listen_port, const char *remote_ip, uint16_t remote_port)
{
    proxy_config_t *cfg = (proxy_config_t *)malloc(sizeof(proxy_config_t));
    if (!cfg) {
        return;
    }

    cfg->listen_port = listen_port;
    cfg->remote_port = remote_port;

    strncpy(cfg->remote_ip, remote_ip, sizeof(cfg->remote_ip) - 1);
    cfg->remote_ip[sizeof(cfg->remote_ip) - 1] = '\0';

    xTaskCreate(
        proxy_task,
        "proxy",
        PROXY_STACK_SIZE,
        cfg,
        PROXY_PRIORITY,
        NULL);
}