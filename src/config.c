#include "config.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define STR_TO_NUM_BASE 10

static bool str_to_num(const char *str, unsigned long *result, unsigned long min,
                       unsigned long max) {
  errno = 0;
  char *end;
  unsigned long v = strtoul(str, &end, STR_TO_NUM_BASE);
  if (errno != 0 || end == str || *end != '\0' || v < min || v > max) {
    return false;
  }
  *result = v;
  return true;
}

static const char *get_str_config(const char *name, const char *default_value) {
  const char *env_config = getenv(name);
  return (env_config != NULL) ? env_config : default_value;
}

static unsigned long get_num_config(const char *name, unsigned long default_value,
                                    unsigned long min_value, unsigned long max_value) {
  const char *env_config = getenv(name);
  if (env_config != NULL) {
    unsigned long value;
    if (str_to_num(env_config, &value, min_value, max_value)) {
      return value;
    }
    (void)fprintf(stderr, "invalid %s=%s, using default %lu\n", name, env_config, default_value);
  }
  return default_value;
}

const char *get_address(void) {
  return get_str_config("KVS_ADDRESS", KVS_DEFAULT_ADDRESS);
}

uint16_t get_port(void) {
  return (uint16_t)get_num_config("KVS_PORT", KVS_DEFAULT_PORT, 0, UINT16_MAX);
}
