#include "config.h"

#include <stdint.h>

const char *get_address(void) {
  return KVS_DEFAULT_ADDRESS;
}

uint16_t get_port(void) {
  return KVS_DEFAULT_PORT;
}
