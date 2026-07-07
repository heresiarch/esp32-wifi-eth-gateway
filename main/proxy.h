#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start a TCP proxy.
 *
 * Listens on listen_port and forwards all traffic to
 * remote_ip:remote_port.
 *
 * Only one client is served at a time.
 */
void proxy_start(uint16_t listen_port,
                 const char *remote_ip,
                 uint16_t remote_port);

#ifdef __cplusplus
}
#endif