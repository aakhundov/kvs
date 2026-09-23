#ifndef KVS_CONFIG_H
#define KVS_CONFIG_H

#include <stdint.h>

#ifndef KVS_DEFAULT_ADDRESS
#define KVS_DEFAULT_ADDRESS "127.0.0.1"
#endif

#ifndef KVS_DEFAULT_PORT
#define KVS_DEFAULT_PORT 7777
#endif

#ifndef KVS_DEFAULT_MAX_CONNECTIONS
#define KVS_DEFAULT_MAX_CONNECTIONS 256
#endif

#ifndef KVS_DEFAULT_STOP_TIMEOUT
#define KVS_DEFAULT_STOP_TIMEOUT 10 // seconds
#endif

const char *get_address(void);
uint16_t get_port(void);
uint16_t get_max_connections(void);
uint16_t get_stop_timeout(void);

#endif
