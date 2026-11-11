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
 * Only one client is served at a time per instance.
 *
 * This function may be called several times to run independent proxy
 * instances. Each call allocates its own task, configuration and buffer,
 * so instances do not share state. Each instance must use a different
 * listen_port.
 */
void proxy_start(uint16_t listen_port,
                 const char *remote_ip,
                 uint16_t remote_port);

#ifdef __cplusplus
}
#endif