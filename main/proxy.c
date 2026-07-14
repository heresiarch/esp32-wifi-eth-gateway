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

#define PROXY_STACK_SIZE 3072 // Reduziert, da der Puffer auf dem Heap liegt
#define PROXY_PRIORITY   5
#define BUFFER_SIZE      1460

typedef struct
{
    uint16_t listen_port;
    uint16_t remote_port;
    char remote_ip[32];
} proxy_config_t;

/*---------------------------------------------------------------
 * Hilfsfunktionen für Socket-Sicherheit & TCP-Verwaltung
 *-------------------------------------------------------------*/

// Konfiguriert Sockets gegen Blockaden, Time-Outs und "Zombie-Verbindungen"
static void configure_socket_safety(int fd) {
    // 1. Non-blocking Modus aktivieren (für asynchrones select())
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags != -1) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    // 2. TCP Keep-Alive aktivieren (erkennt Kabel-Trennen oder tote Gegenstellen)
    int keepalive = 1;
    int keepidle = 10;   // 10s Inaktivität, bevor Sonden gesendet werden
    int keepintvl = 2;   // Alle 2s eine Sonde senden
    int keepcnt = 3;     // Nach 3 unbeantworteten Sonden Verbindung trennen
    
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));

    // 3. SO_LINGER konfigurieren
    // Verhindert, dass geschlossene Sockets minutenlang im Zustand TIME_WAIT verharren.
    // l_onoff = 1, l_linger = 0 erzwingt beim Schließen einen direkten TCP-RST (Reset).
    struct linger sl = { .l_onoff = 1, .l_linger = 0 }; 
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &sl, sizeof(sl));
}

// Sendet Daten blockierungsfrei und fängt Teilschreibvorgänge (Partial Writes) ab
static int send_all(int fd, const uint8_t *buf, int len) {
    int total_sent = 0;
    while (total_sent < len) {
        int sent = send(fd, buf + total_sent, len - total_sent, 0);
        if (sent <= 0) {
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                vTaskDelay(pdMS_TO_TICKS(5)); // Kurz abgeben und erneut versuchen
                continue;
            }
            return -1; // Allgemeiner Verbindungsfehler beim Senden
        }
        total_sent += sent;
    }
    return total_sent;
}

/*---------------------------------------------------------------
 * Haupt-Proxy-Task
 *-------------------------------------------------------------*/
static void proxy_task(void *arg)
{
    proxy_config_t *cfg = (proxy_config_t *)arg;
    uint8_t *buffer = malloc(BUFFER_SIZE);
    if (!buffer) {
        ESP_LOGE(TAG, "Kritisch: Speicherzuweisung für Netzwerk-Puffer fehlgeschlagen!");
        goto exit;
    }

    struct sockaddr_in listen_addr = {};
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(cfg->listen_port);
    listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "socket() fehlgeschlagen");
        goto free_buffer;
    }

    int opt = 1;
    // SO_REUSEADDR erlaubt das sofortige Wiederbinden des Ports nach einem Reset
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(listen_sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) != 0) {
        ESP_LOGE(TAG, "bind() fehlgeschlagen");
        goto cleanup_listener;
    }

    if (listen(listen_sock, 1) != 0) {
        ESP_LOGE(TAG, "listen() fehlgeschlagen");
        goto cleanup_listener;
    }

    ESP_LOGI(TAG, "Proxy lauscht auf Port %u", cfg->listen_port);

    while (true) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client = accept(listen_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (client < 0) {
            vTaskDelay(pdMS_TO_TICKS(10)); // Watchdog füttern
            continue;
        }

        ESP_LOGI(TAG, "Client verbunden!");
        led_set_max_brightness(100);
        led_set_blink_color(0, 255, 255); // Cyan

        struct sockaddr_in remote_addr = {};
        remote_addr.sin_family = AF_INET;
        remote_addr.sin_port = htons(cfg->remote_port);
        inet_pton(AF_INET, cfg->remote_ip, &remote_addr.sin_addr);

        int server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (server < 0) {
            ESP_LOGE(TAG, "Konnte Server-Socket nicht erstellen");
            close(client); // Wichtig! Offenen Client-Socket direkt wieder schließen
            continue;
        }

        // Setze Lese-/Schreib-Timeout für den Verbindungsaufbau (Standardmäßig blockierend)
        struct timeval conn_timeout = { .tv_sec = 5, .tv_usec = 0 }; // 5 Sekunden Timeout
        setsockopt(server, SOL_SOCKET, SO_SNDTIMEO, &conn_timeout, sizeof(conn_timeout));

        if (connect(server, (struct sockaddr *)&remote_addr, sizeof(remote_addr)) != 0) {
            ESP_LOGE(TAG, "Konnte keine Verbindung zu %s:%u aufbauen", cfg->remote_ip, cfg->remote_port);
            close(client); // Sicherer Abbau
            close(server); // Sicherer Abbau
            continue;
        }

        // Sockets für den sicheren Datentransfer konfigurieren (Non-blocking, Keep-Alive, Linger)
        configure_socket_safety(client);
        configure_socket_safety(server);

        ESP_LOGI(TAG, "Verbindung zum Ziel-Server steht.");

        while (true) {
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(client, &readfds);
            FD_SET(server, &readfds);

            int maxfd = client > server ? client : server;

            // Lese-Inaktivitäts-Timeout: Nach 30 Sekunden Stille trennen wir die Verbindung,
            // um "tote Leitungen" zu eliminieren (schützt vor Socket-Vollauf).
            struct timeval timeout = { .tv_sec = 30, .tv_usec = 0 };
            int ret = select(maxfd + 1, &readfds, NULL, NULL, &timeout);

            if (ret < 0) {
                break; // Select-Fehler -> Schleife verlassen
            }
            if (ret == 0) {
                ESP_LOGW(TAG, "Inaktivitäts-Timeout (30s) erreicht. Schließe Verbindung.");
                break; // Timeout -> Abbruch
            }

            // Fall 1: Client hat Daten gesendet -> Weiterleiten an Server
            if (FD_ISSET(client, &readfds)) {
                int len = recv(client, buffer, BUFFER_SIZE, 0);
                if (len <= 0) {
                    if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        continue;
                    }
                    break; // Client hat Verbindung geschlossen oder Error
                }
                if (send_all(server, buffer, len) < 0) {
                    break;
                }
            }

            // Fall 2: Server hat Daten gesendet -> Weiterleiten an Client
            if (FD_ISSET(server, &readfds)) {
                int len = recv(server, buffer, BUFFER_SIZE, 0);
                if (len <= 0) {
                    if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        continue;
                    }
                    break; // Server hat Verbindung geschlossen oder Error
                }
                if (send_all(client, buffer, len) < 0) {
                    break;
                }
            }
        }

        ESP_LOGI(TAG, "Verbindung beendet. Räume Sockets auf...");
        led_set_max_brightness(100);
        led_set_blink_color(255, 0, 0); // Rot

        // Beide Sockets geordnet schließen & Ressourcen freigeben
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
 * Start-Funktion
 *-------------------------------------------------------------*/
void proxy_start(uint16_t listen_port, const char *remote_ip, uint16_t remote_port)
{
    proxy_config_t *cfg = (proxy_config_t *)malloc(sizeof(proxy_config_t));
    if (!cfg) {
        ESP_LOGE(TAG, "Konnte Konfigurationsstruktur nicht erstellen!");
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
