#include "logger.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

// A hang detector, not synchronisation: a scenario finishes in
// milliseconds, so the parent only waits this long when the child is stuck
// (a logger that never returns from its stop, a producer spinning on a ring
// nobody drains).
#define KVS_TEST_LOG_TIMEOUT_MS 5000

static void keep(kvs_test_log_t *log, const char *bytes, size_t n) {
  size_t room = sizeof(log->text) - log->len - 1;
  if (n > room) {
    n = room;
    log->truncated = true;
  }
  memcpy(log->text + log->len, bytes, n);
  log->len += n;
  log->text[log->len] = '\0';
}

static void run_child(int write_fd, kvs_test_log_scenario_t *scenario, void *arg) {
  if (dup2(write_fd, STDERR_FILENO) < 0) {
    _exit(127);
  }
  close(write_fd);
  // exit, not _exit: the sanitizers' reports (LeakSanitizer's above all)
  // are produced at exit and have to reach the pipe
  exit(scenario(arg));
}

// reads the child's stderr to end of input into (log). a child that goes
// quiet for longer than the bound is killed, which closes the pipe.
static void read_all(kvs_test_log_t *log, int fd, pid_t pid) {
  char chunk[4096];
  while (true) {
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    int ready = poll(&pfd, 1, KVS_TEST_LOG_TIMEOUT_MS);
    if (ready < 0 && errno == EINTR) {
      continue;
    }
    if (ready == 0) {
      const char *note = "[harness] child went quiet: sending SIGKILL\n";
      keep(log, note, strlen(note));
      kill(pid, SIGKILL);
      continue; // the kill closes the pipe: read on to end of input
    }

    ssize_t n = read(fd, chunk, sizeof(chunk));
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      return;
    }
    keep(log, chunk, (size_t)n);
  }
}

bool kvs_test_log_run(kvs_test_log_t *log, kvs_test_log_scenario_t *scenario, void *arg) {
  log->status = -1;
  log->truncated = false;
  log->len = 0;
  log->text[0] = '\0';

  int fds[2];
  if (pipe(fds) < 0) {
    return false;
  }

  // the child inherits stdio's buffers: flushed here, or the child's exit
  // would write the parent's pending output a second time
  (void)fflush(NULL);

  pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return false;
  }
  if (pid == 0) {
    close(fds[0]);
    run_child(fds[1], scenario, arg);
  }

  close(fds[1]);
  read_all(log, fds[0], pid);
  close(fds[0]);

  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  log->status = status;
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

void kvs_test_log_dump(const kvs_test_log_t *log) {
  (void)fprintf(stderr, "---- the child said%s:\n%s", log->truncated ? " (truncated)" : "",
                log->text);
  if (WIFEXITED(log->status)) {
    (void)fprintf(stderr, "---- the child exited with status %d\n", WEXITSTATUS(log->status));
  } else if (WIFSIGNALED(log->status)) {
    (void)fprintf(stderr, "---- the child was killed by signal %d\n", WTERMSIG(log->status));
  }
}

int kvs_test_log_fail(const char *what) {
  (void)fprintf(stderr, "[scenario] %s\n", what);
  return 1;
}
