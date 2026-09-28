#ifndef KVS_TEST_LOGGER_H
#define KVS_TEST_LOGGER_H

#include <stdbool.h>
#include <stddef.h>

// The logger harness: a scenario runs in a forked child with its standard
// error on a pipe, so the logger's process-wide state (the pool, the logger
// thread) is fresh in every test, and every line any thread of the child
// writes reaches the parent. The parent reads the pipe to end of input,
// reaps the child, and keeps what it said; tests print it only when they
// fail, so a passing run is silent.
//
// A scenario reports its own checks through its return value, which
// becomes the child's exit status: 0 means every check held. A failing
// check writes one line saying which to standard error, so the dump shows
// it among the logger's lines.

#define KVS_TEST_LOG_SIZE (256 * 1024)

typedef struct kvs_test_log_t {
  int status;     // as waitpid wrote it
  bool truncated; // the child said more than (text) holds
  char text[KVS_TEST_LOG_SIZE];
  size_t len;
} kvs_test_log_t;

typedef int kvs_test_log_scenario_t(void *arg);

// runs (scenario) with (arg) in a child and captures its standard error in
// (log). returns true if the child exited normally with status 0. a child
// that goes quiet for longer than a hang bound is killed and fails.
bool kvs_test_log_run(kvs_test_log_t *log, kvs_test_log_scenario_t *scenario, void *arg);

// prints everything the child said to stderr: the diagnosis, for a failure.
void kvs_test_log_dump(const kvs_test_log_t *log);

// for use inside a scenario: writes "[scenario] (what)" to standard error
// and returns 1, the exit status of a failed check.
int kvs_test_log_fail(const char *what);

#endif
