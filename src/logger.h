#ifndef KVS_DEBUG_H
#define KVS_DEBUG_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>

#ifndef KVS_LOG_LEVEL
#define KVS_LOG_LEVEL 0
#endif

// topic needs to outlive the call when logging asynchronously
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

// init / free must be called once before and after, respectively,
// any logging calls (to the macros above) made from any threads.
// there should be at least as many ring buffers as the maximum
// number of alive threads writing their logs asynchronously.
// init returns false on a resource failure.
bool kvs_logger_init(size_t num_ring_buffers);
void kvs_logger_free(bool free_ring_buffers);

// to write its logs asynchronously (not to the stderr directly
// inside the logging call), a thread needs to call attach / detach
// once before and after writing any log records, respectively. the
// functions are idempotent. attach returns false if there's no
// available ring buffer: subsequent logging calls will write
// synchronously, unless another attach call returns true.
bool kvs_logger_async_attach(void);
void kvs_logger_async_detach(void);

// not to be called directly: use the logging macros instead.
__attribute__((format(printf, 3, 4))) void kvs_print(const char *topic, int id, const char *fmt,
                                                     ...);

#endif
