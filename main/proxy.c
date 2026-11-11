#include "proxy.h"
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#include "led.h"

static const char *TAG = "PROXY";

// Increased to 4096 to avoid stack overflows caused by lwIP/ESP_LOG
#define PROXY_STACK_SIZE 4096 
#define PROXY_PRIORITY   5
#define BUFFER_SIZE      1460
#define SEND_TIMEOUT_MS  5000 // Wait at most 5 seconds for a blocking write buffer

// Per-instance logging: every message carries the owning listen port
#define PLOGI(port, fmt, ...) ESP_LOGI(TAG, "[port %u] " fmt, (unsigned)(port), ##__VA_ARGS__)
#define PLOGW(port, fmt, ...) ESP_LOGW(TAG, "[port %u] " fmt, (unsigned)(port), ##__VA_ARGS__)
#define PLOGE(port, fmt, ...) ESP_LOGE(TAG, "[port %u] " fmt, (unsigned)(port), ##__VA_ARGS__)

typedef struct
{
    uint16_t listen_port;
    uint16_t remote_port;
    char remote_ip[32];
} proxy_config_t;

/*---------------------------------------------------------------
 * Helper functions for socket safety & TCP management
 *-------------------------------------------------------------*/

static void configure_socket_safety(int fd) {
    // 1. Enable non-blocking mode
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags != -1) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    // 2. Enable TCP keep-alive
    int keepalive = 1;
    int keepidle = 10;   
    int keepintvl = 2;   
    int keepcnt = 3;     
    
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));

    // NOTE: SO_LINGER with l_linger = 0 was removed.
    // A clean TCP teardown (FIN) is more reliable for data integrity.
}

// Sends data without blocking, with a built-in timeout guard against stalls
static int send_all(uint16_t listen_port, int fd, const uint8_t *buf, int len) {
    int total_sent = 0;
    int attempts = 0;
    const int max_attempts = SEND_TIMEOUT_MS / 10; // e.g. 500 attempts of 10 ms

    while (total_sent < len) {
        int sent = send(fd, buf + total_sent, len - total_sent, 0);
        if (sent <= 0) {
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (++attempts > max_attempts) {
                    PLOGW(listen_port, "Write timeout reached on socket %d.", fd);
                    return -1; // Abort on timeout
                }
                vTaskDelay(pdMS_TO_TICKS(10)); 
                continue;
            }
            return -1; 
        }
        total_sent += sent;
        attempts = 0; // Reset counter on success
    }
    return total_sent;
}

/*---------------------------------------------------------------
 * Main proxy task
 *-------------------------------------------------------------*/
static void proxy_task(void *arg)
{
    proxy_config_t *cfg = (proxy_config_t *)arg;
    uint8_t *buffer = malloc(BUFFER_SIZE);
    if (!buffer) {
        PLOGE(cfg->listen_port, "startup failed: allocation (buffer)");
        goto exit;
    }

    struct sockaddr_in listen_addr = {};
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(cfg->listen_port);
    listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock < 0) {
        PLOGE(cfg->listen_port, "startup failed: allocation (socket)");
        goto free_buffer;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(listen_sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) != 0) {
        PLOGE(cfg->listen_port, "startup failed: bind (errno %d)", errno);
        goto cleanup_listener;
    }

    if (listen(listen_sock, 1) != 0) {
        PLOGE(cfg->listen_port, "startup failed: listen (errno %d)", errno);
        goto cleanup_listener;
    }

    PLOGI(cfg->listen_port, "Proxy listening");

    while (true) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client = accept(listen_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (client < 0) {
            vTaskDelay(pdMS_TO_TICKS(50)); // Slightly increased to reduce CPU load on errors
            continue;
        }

        PLOGI(cfg->listen_port, "Client connected!");
        led_set_max_brightness(100);
        led_set_blink_color(0, 255, 255); 

        struct sockaddr_in remote_addr = {};
        remote_addr.sin_family = AF_INET;
        remote_addr.sin_port = htons(cfg->remote_port);
        
        if (inet_pton(AF_INET, cfg->remote_ip, &remote_addr.sin_addr) != 1) {
            PLOGE(cfg->listen_port, "Invalid IP address: %s", cfg->remote_ip);
            close(client);
            continue;
        }

        int server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (server < 0) {
            PLOGE(cfg->listen_port, "Could not create server socket");
            close(client); 
            continue;
        }

        struct timeval conn_timeout = { .tv_sec = 5, .tv_usec = 0 }; 
        setsockopt(server, SOL_SOCKET, SO_SNDTIMEO, &conn_timeout, sizeof(conn_timeout));

        if (connect(server, (struct sockaddr *)&remote_addr, sizeof(remote_addr)) != 0) {
            PLOGE(cfg->listen_port, "Could not connect to %s:%u", cfg->remote_ip, cfg->remote_port);
            close(client); 
            close(server); 
            continue;
        }

        configure_socket_safety(client);
        configure_socket_safety(server);

        PLOGI(cfg->listen_port, "Connection to target server established.");

        while (true) {
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(client, &readfds);
            FD_SET(server, &readfds);

            int maxfd = client > server ? client : server;
            struct timeval timeout = { .tv_sec = 30, .tv_usec = 0 };
            int ret = select(maxfd + 1, &readfds, NULL, NULL, &timeout);

            if (ret < 0) {
                PLOGE(cfg->listen_port, "select() error, closing sockets.");
                break; 
            }
            if (ret == 0) {
                PLOGW(cfg->listen_port, "Inactivity timeout (30s) reached. Closing connection.");
                break; 
            }

            if (FD_ISSET(client, &readfds)) {
                int len = recv(client, buffer, BUFFER_SIZE, 0);
                if (len <= 0) {
                    if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        continue;
                    }
                    break; 
                }
                if (send_all(cfg->listen_port, server, buffer, len) < 0) {
                    break;
                }
            }

            if (FD_ISSET(server, &readfds)) {
                int len = recv(server, buffer, BUFFER_SIZE, 0);
                if (len <= 0) {
                    if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        continue;
                    }
                    break; 
                }
                if (send_all(cfg->listen_port, client, buffer, len) < 0) {
                    break;
                }
            }
        }

        PLOGI(cfg->listen_port, "Connection closed. Cleaning up sockets...");
        led_set_max_brightness(100);
        led_set_blink_color(255, 0, 0); 

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

/*---------------------------------------------------------------
 * Start function
 *-------------------------------------------------------------*/
void proxy_start(uint16_t listen_port, const char *remote_ip, uint16_t remote_port)
{
    proxy_config_t *cfg = (proxy_config_t *)malloc(sizeof(proxy_config_t));
    if (!cfg) {
        PLOGE(listen_port, "startup failed: allocation (config)");
        return;
    }

    cfg->listen_port = listen_port;
    cfg->remote_port = remote_port;

    strncpy(cfg->remote_ip, remote_ip, sizeof(cfg->remote_ip) - 1);
    cfg->remote_ip[sizeof(cfg->remote_ip) - 1] = '\0';

    // xTaskCreate copies the name, so a stack buffer is safe
    char name[16];
    snprintf(name, sizeof(name), "proxy_%u", (unsigned)listen_port);

    BaseType_t result = xTaskCreate(
        proxy_task,
        name,
        PROXY_STACK_SIZE,
        cfg,
        PROXY_PRIORITY,
        NULL);

    // Fixes a memory leak if the task fails to start
    if (result != pdPASS) {
        PLOGE(listen_port, "startup failed: allocation (task)");
        free(cfg);
    }
}
