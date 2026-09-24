#ifndef KVS_CONFIG_H
#define KVS_CONFIG_H

#include <stdint.h>

#define KVS_DEFAULT_ADDRESS "127.0.0.1"
#define KVS_DEFAULT_PORT 7777
#define KVS_DEFAULT_MAX_CONNECTIONS 256
#define KVS_DEFAULT_STOP_TIMEOUT 10 // seconds

const char *get_address(void);
uint16_t get_port(void);
uint16_t get_max_connections(void);
uint16_t get_stop_timeout(void);

#endif
