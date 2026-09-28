#include "logger.h"

#include <assert.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <pthread.h>

#define CACHE_LINE 128
#define NUM_RING_SLOTS 64
#define MAX_LOG_LENGTH 255
#define LOGGER_SLEEP_SEC 0.001

typedef struct kvs_record_t {
  char message[MAX_LOG_LENGTH + 1];
  const char *topic; // assumed alive
  int id;
} kvs_record_t;

typedef struct kvs_ring_t {
  // consumer's line
  _Alignas(CACHE_LINE) atomic_size_t head;
  // producer's line
  _Alignas(CACHE_LINE) atomic_size_t tail;
  size_t head_seen;
  // next line (after producer's)
  _Alignas(CACHE_LINE) bool attached;
  kvs_record_t slots[NUM_RING_SLOTS];
} kvs_ring_t;

static size_t num_rings = 0;
static kvs_ring_t *rings = NULL;
static _Thread_local kvs_ring_t *my_ring = NULL;

// protects access to the rings' (attached) field
static pthread_mutex_t attach_log = PTHREAD_MUTEX_INITIALIZER;

static pthread_t logger_thread;
static atomic_bool stop;

static inline void format_record(kvs_record_t *rec, const char *topic, int id, const char *fmt,
                                 va_list ap) {
  int len = vsnprintf(rec->message, MAX_LOG_LENGTH + 1, fmt, ap);
  if (len > MAX_LOG_LENGTH && MAX_LOG_LENGTH >= 3) {
    // truncate incomplete log message with with ...
    memset(rec->message + (MAX_LOG_LENGTH - 3), '.', 3);
  }

  rec->topic = topic;
  rec->id = id;
}

static inline void print_record(const kvs_record_t *rec) {
  if (rec->id == INT_MAX) {
    (void)fprintf(stderr, "---- %s: %s\n", rec->topic, rec->message);
  } else {
    (void)fprintf(stderr, "---- [%d] %s: %s\n", rec->id, rec->topic, rec->message);
  }
}

static inline void print_now(const char *topic, int id, const char *fmt, va_list ap) {
  kvs_record_t local_rec;
  format_record(&local_rec, topic, id, fmt, ap);
  print_record(&local_rec);
}

static void add_to_ring(kvs_ring_t *r, const char *topic, int id, const char *fmt, va_list ap) {
  size_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
  while (tail - r->head_seen == NUM_RING_SLOTS) {
    // busy-wait until a slot becomes available: to get the slot ASAP
    r->head_seen = atomic_load_explicit(&r->head, memory_order_acquire);
  }
  format_record(&r->slots[tail % NUM_RING_SLOTS], topic, id, fmt, ap);
  // reveal the slot to consumer as soon as the record is prepared
  atomic_store_explicit(&r->tail, tail + 1, memory_order_release);
}

static size_t drain_ring(kvs_ring_t *r) {
  size_t printed = 0;
  size_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
  size_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
  while (head < tail) {
    print_record(&r->slots[head % NUM_RING_SLOTS]);
    // vacate the slot for producer as soon as the record is printed
    atomic_store_explicit(&r->head, ++head, memory_order_release);
    printed++;
    // tail is not read again here to give equal treatment to all
    // rings and avoid a situation where one thread spams its ring
    // and makes the log records from other threads / rings never
    // printed or printed late (and the logger thread not respond
    // to the stop flag)
  }
  return printed;
}

static inline size_t drain_all_rings(void) {
  size_t total_printed = 0;
  for (kvs_ring_t *r = rings; r < rings + num_rings; r++) {
    total_printed += drain_ring(r);
  }
  return total_printed;
}

#define NS_IN_SEC (1000L * 1000L * 1000L)

static inline struct timespec make_ts(long total_ns) {
  assert(total_ns >= 0);

  return (struct timespec){
      .tv_sec = total_ns / NS_IN_SEC,
      .tv_nsec = total_ns % NS_IN_SEC,
  };
}

static void *logger(void *arg) {
  (void)arg;

  long sleep_ns = (long)(LOGGER_SLEEP_SEC * NS_IN_SEC);
  struct timespec sleep_time = make_ts(sleep_ns);

  while (true) {
    if (atomic_load(&stop)) {
      drain_all_rings();
      break;
    }

    if (drain_all_rings() > 0) {
      continue;
    }

    nanosleep(&sleep_time, NULL);
  }

  return NULL;
}

bool kvs_logger_init(size_t num_ring_buffers) {
  assert(rings == NULL);

  num_rings = num_ring_buffers;
  rings = aligned_alloc(_Alignof(kvs_ring_t), num_rings * sizeof(*rings));
  if (rings == NULL) {
    return false; // allocation failed
  }
  // zero out the rings' memory to init fields
  memset(rings, 0, num_rings * sizeof(*rings));

  sigset_t old_signals;
  sigset_t all_signals;
  sigfillset(&all_signals);

  // block all signals in the logger thread
  pthread_sigmask(SIG_BLOCK, &all_signals, &old_signals);

  atomic_store(&stop, false);
  bool thread_not_created = false;
  if (pthread_create(&logger_thread, NULL, logger, NULL) != 0) {
    thread_not_created = true;
  }

  // restore signals in the main thread
  pthread_sigmask(SIG_SETMASK, &old_signals, NULL);

  if (thread_not_created) {
    free(rings);
    rings = NULL;
    return false;
  }

  return true;
}

void kvs_logger_free(bool free_ring_buffers) {
  assert(rings != NULL);

  atomic_store(&stop, true);
  pthread_join(logger_thread, NULL);

  if (free_ring_buffers) {
    free(rings);
    rings = NULL;
  }
}

bool kvs_logger_async_attach(void) {
  if (my_ring != NULL) {
    return true;
  }

  bool succeeded = false;
  pthread_mutex_lock(&attach_log);

  for (kvs_ring_t *r = rings; r < rings + num_rings; r++) {
    if (!r->attached) {
      my_ring = r;
      my_ring->attached = true;
      succeeded = true;
      break;
    }
  }

  pthread_mutex_unlock(&attach_log);
  return succeeded;
}

void kvs_logger_async_detach(void) {
  if (my_ring == NULL) {
    return;
  }

  pthread_mutex_lock(&attach_log);

  my_ring->attached = false;
  my_ring = NULL;

  pthread_mutex_unlock(&attach_log);
}

void kvs_print(const char *topic, int id, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);

  if (my_ring != NULL) {
    add_to_ring(my_ring, topic, id, fmt, ap);
  } else {
    print_now(topic, id, fmt, ap);
  }

  va_end(ap);
}
