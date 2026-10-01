#include "debug.h"

#include <assert.h>
#include <stdbool.h>
#include <time.h>

#include "config.h"

static _Thread_local bool my_state_init = false;
static _Thread_local unsigned long my_state;

#define RNG_INIT_FACTOR 11400714819323198485UL
#define RNG_NEXT_FACTOR 6364136223846793005UL
#define RNG_NEXT_TERM 1442695040888963407UL
#define RNG_NEXT_SHIFT_BITS 33

static inline unsigned long get_init_state(unsigned long stream) {
  return get_stress_seed() ^ (stream * RNG_INIT_FACTOR);
}

static inline unsigned long next_random(void) {
  my_state = (my_state * RNG_NEXT_FACTOR) + RNG_NEXT_TERM;
  return my_state >> RNG_NEXT_SHIFT_BITS;
}

#define NS_IN_SEC 1000000000

static inline double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + ((double)t.tv_nsec / NS_IN_SEC);
}

static inline void busy_wait(long ns) {
  double deadline = now() + ((double)ns / NS_IN_SEC);
  while (now() < deadline) {
    // the loop doesn't compile out due to the
    // external library call in the condition
  }
}

#define SLEEP_BOUND_US 64
#define NS_IN_US 1000

void kvs_sched_point(void) {
  assert(my_state_init);

  unsigned long r = next_random();
  if (r % 2 != 0) { // don't sleep half of the time
    long sleep_ns = (long)(r % SLEEP_BOUND_US) * NS_IN_US;
    busy_wait(sleep_ns); // more precise than sleep
  }
}

void kvs_set_thread_stream(unsigned long stream) {
  assert(!my_state_init);

  my_state = get_init_state(stream);
  my_state_init = true;
}
