#ifndef KVS_DEBUG_H
#define KVS_DEBUG_H

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifndef KVS_LOG_LEVEL
#define KVS_LOG_LEVEL 0
#endif

#define KVS_MAX_LOG_MSG_LENGTH 255

#define KVS_PRINT(topic, ...) kvs_print(topic, INT_MAX, __VA_ARGS__)
#define KVS_PRINT_WITH_ID(topic, id, ...) kvs_print(topic, id, __VA_ARGS__)

#if KVS_LOG_LEVEL > 0
#define KVS_LOG(...) KVS_PRINT(__VA_ARGS__)
#define KVS_LOG_WITH_ID(...) KVS_PRINT_WITH_ID(__VA_ARGS__)
#if KVS_LOG_LEVEL > 1
#define KVS_TRACE(...) KVS_PRINT(__VA_ARGS__)
#define KVS_TRACE_WITH_ID(...) KVS_PRINT_WITH_ID(__VA_ARGS__)
#else
#define KVS_TRACE(...) (void)0
#define KVS_TRACE_WITH_ID(...) (void)0
#endif
#else
#define KVS_LOG(...) (void)0
#define KVS_LOG_WITH_ID(...) (void)0
#define KVS_TRACE(...) (void)0
#define KVS_TRACE_WITH_ID(...) (void)0
#endif

static inline __attribute__((format(printf, 3, 4))) int kvs_print(const char *topic, int id,
                                                                  const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char message[KVS_MAX_LOG_MSG_LENGTH + 1];
  int len = vsnprintf(message, sizeof(message), fmt, ap);
  va_end(ap);

  if (len > KVS_MAX_LOG_MSG_LENGTH) {
    // truncate incomplete log message with with ...
    memset(message + (KVS_MAX_LOG_MSG_LENGTH - 3), '.', 3);
  }

  if (id == INT_MAX) {
    return fprintf(stderr, "---- %s: %s\n", topic, message);
  }
  return fprintf(stderr, "---- [%d] %s: %s\n", id, topic, message);
}

#endif
