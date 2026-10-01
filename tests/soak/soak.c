#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <dirent.h>
#include <pthread.h>
#include <unistd.h>

#include "server.h"

#include "../support/server.h"

// Runs one server under an ordinary load for a fixed time and watches its
// resources: descriptors and threads must come back to where they were
// when the server was idle, and resident memory must stop growing once the
// store holds every key the load uses. Silent when it passes.

#define SOAK_MS 10000
#define WARMUP_MS 1000
#define SAMPLE_MS 500
#define QUIET_WAIT_MS 2000
#define QUIET_STEP_MS 10
#define MAX_SAMPLES ((SOAK_MS / SAMPLE_MS) + 4)

#define CLIENTS 8
#define STEADY_CLIENTS 2
#define KEYS 1000
#define MAX_SESSION_REQUESTS 8
#define RSS_SLACK_PERCENT 10

#define REPLY_CAP (KVS_MAX_LINE_LENGTH + 1)
#define REQUEST_CAP 64
#define ERROR_CAP 256
#define PATH_CAP 64

typedef struct sample_t {
  long ms;
  int fds;
  int threads;
  long rss_kib;
} sample_t;

typedef struct client_t {
  pthread_t thread;
  const kvs_test_server_t *server;
  bool steady; // one connection for the whole run, not one per session
  unsigned long state;
  char error[ERROR_CAP]; // why the client stopped early; empty if it did not
} client_t;

static double start_ms;
static double deadline_ms;

static double now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return ((double)t.tv_sec * 1e3) + ((double)t.tv_nsec / 1e6);
}

static void pause_ms(long ms) {
  struct timespec d = {ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&d, NULL);
}

static unsigned long next_random(unsigned long *state) {
  *state ^= *state << 13;
  *state ^= *state >> 7;
  *state ^= *state << 17;
  return *state;
}

// the entries of a /proc directory, not counting . and ..; -1 on failure
static int count_entries(const char *path) {
  DIR *dir = opendir(path);
  if (dir == NULL) {
    return -1;
  }
  int n = 0;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_name[0] != '.') {
      n++;
    }
  }
  closedir(dir);
  return n;
}

static long rss_kib(pid_t pid) {
  char path[PATH_CAP];
  (void)snprintf(path, sizeof path, "/proc/%d/status", (int)pid);
  FILE *status = fopen(path, "r");
  if (status == NULL) {
    return -1;
  }
  long kib = -1;
  char line[256];
  while (fgets(line, sizeof line, status) != NULL) {
    if (strncmp(line, "VmRSS:", 6) == 0) {
      kib = strtol(line + 6, NULL, 10);
      break;
    }
  }
  (void)fclose(status);
  return kib;
}

static bool take_sample(pid_t pid, sample_t *s) {
  char path[PATH_CAP];
  s->ms = (long)(now_ms() - start_ms);
  (void)snprintf(path, sizeof path, "/proc/%d/fd", (int)pid);
  s->fds = count_entries(path);
  (void)snprintf(path, sizeof path, "/proc/%d/task", (int)pid);
  s->threads = count_entries(path);
  s->rss_kib = rss_kib(pid);
  return s->fds >= 0 && s->threads >= 0 && s->rss_kib >= 0;
}

// sends (request) and checks that the reply starts with one of the two
// prefixes (the second may be NULL). records the reason on failure
static bool expect(client_t *c, int fd, const char *request, const char *first,
                   const char *second) {
  char reply[REPLY_CAP];
  if (!kvs_test_request(fd, request, reply, sizeof reply)) {
    (void)snprintf(c->error, sizeof c->error, "no reply to [%s]", request);
    return false;
  }
  if (strncmp(reply, first, strlen(first)) == 0 ||
      (second != NULL && strncmp(reply, second, strlen(second)) == 0)) {
    return true;
  }
  (void)snprintf(c->error, sizeof c->error, "[%s] got [%s]", request, reply);
  return false;
}

// a line longer than the server accepts, which it answers with an error
static bool over_long_request(client_t *c, int fd) {
  char line[KVS_MAX_LINE_LENGTH + 16];
  memset(line, 'x', sizeof line - 1);
  line[sizeof line - 1] = '\n';
  char reply[REPLY_CAP];
  if (!kvs_test_send(fd, line, sizeof line) || kvs_test_recv_line(fd, reply, sizeof reply) < 0) {
    (void)snprintf(c->error, sizeof c->error, "no reply to an over-long line");
    return false;
  }
  if (strncmp(reply, "ERR ", 4) != 0) {
    (void)snprintf(c->error, sizeof c->error, "an over-long line got [%s]", reply);
    return false;
  }
  return true;
}

// one request, mostly SET and GET over a fixed set of keys, now and then
// a DEL or a request the server rejects
static bool one_request(client_t *c, int fd) {
  unsigned long r = next_random(&c->state);
  unsigned long key = (r >> 8) % KEYS;
  char request[REQUEST_CAP];
  switch (r % 100) {
  case 98:
    return expect(c, fd, "PUT k 1", "ERR ", NULL);
  case 99:
    return over_long_request(c, fd);
  default:
    break;
  }
  if (r % 100 < 45) {
    (void)snprintf(request, sizeof request, "SET k%lu v%lu", key, r % 1000);
    return expect(c, fd, request, "OK", NULL);
  }
  if (r % 100 < 90) {
    (void)snprintf(request, sizeof request, "GET k%lu", key);
    return expect(c, fd, request, "VAL ", "NIL");
  }
  (void)snprintf(request, sizeof request, "DEL k%lu", key);
  return expect(c, fd, request, "OK", "NIL");
}

// a short session: connect, a few requests, leave; one session in a
// hundred leaves in the middle of a request
static bool one_session(client_t *c) {
  int fd = kvs_test_server_connect(c->server);
  if (fd < 0) {
    (void)snprintf(c->error, sizeof c->error, "connect failed");
    return false;
  }
  unsigned long r = next_random(&c->state);
  unsigned long requests = 1 + (r % MAX_SESSION_REQUESTS);
  bool ok = true;
  for (unsigned long i = 0; i < requests && ok; i++) {
    ok = one_request(c, fd);
  }
  if (ok && (r >> 8) % 100 == 0) {
    ok = kvs_test_send(fd, "SET half", 8);
  }
  close(fd);
  return ok;
}

static void *run_client(void *arg) {
  client_t *c = arg;
  if (c->steady) {
    int fd = kvs_test_server_connect(c->server);
    if (fd < 0) {
      (void)snprintf(c->error, sizeof c->error, "connect failed");
      return NULL;
    }
    while (now_ms() < deadline_ms && one_request(c, fd)) {
    }
    close(fd);
  } else {
    while (now_ms() < deadline_ms && one_session(c)) {
    }
  }
  return NULL;
}

static void print_samples(const sample_t *baseline, const sample_t *samples, int n,
                          const sample_t *last) {
  printf("---- soak samples (ms, descriptors, threads, resident KiB):\n");
  printf("idle  %6ld %4d %4d %8ld\n", baseline->ms, baseline->fds, baseline->threads,
         baseline->rss_kib);
  for (int i = 0; i < n; i++) {
    printf("load  %6ld %4d %4d %8ld\n", samples[i].ms, samples[i].fds, samples[i].threads,
           samples[i].rss_kib);
  }
  printf("quiet %6ld %4d %4d %8ld\n", last->ms, last->fds, last->threads, last->rss_kib);
}

int main(void) {
  // one malloc arena: glibc adds arenas as threads come and go, and each
  // adds a step to resident memory that looks like growth and is not
  setenv("MALLOC_ARENA_MAX", "1", 1);

  static kvs_test_server_t server;
  if (!kvs_test_server_spawn(&server)) {
    kvs_test_server_dump(&server);
    return 1;
  }
  pid_t pid = server.pid;

  start_ms = now_ms();
  deadline_ms = start_ms + SOAK_MS;

  sample_t baseline;
  bool sampled = take_sample(pid, &baseline);

  static client_t clients[CLIENTS];
  int started = 0;
  for (int i = 0; i < CLIENTS && sampled; i++) {
    clients[i].server = &server;
    clients[i].steady = i < STEADY_CLIENTS;
    clients[i].state = 0x9E3779B97F4A7C15UL * (unsigned long)(i + 1);
    if (pthread_create(&clients[i].thread, NULL, run_client, &clients[i]) != 0) {
      break;
    }
    started++;
  }

  sample_t samples[MAX_SAMPLES];
  int n = 0;
  long warm_rss = -1;
  while (sampled && started == CLIENTS && now_ms() < deadline_ms && n < MAX_SAMPLES) {
    pause_ms(SAMPLE_MS);
    sampled = take_sample(pid, &samples[n]);
    if (warm_rss < 0 && samples[n].ms >= WARMUP_MS) {
      warm_rss = samples[n].rss_kib;
    }
    n++;
  }
  for (int i = 0; i < started; i++) {
    pthread_join(clients[i].thread, NULL);
  }

  // connection threads end on their own once their clients have left
  sample_t last = baseline;
  for (int waited = 0; sampled && waited < QUIET_WAIT_MS; waited += QUIET_STEP_MS) {
    sampled = take_sample(pid, &last);
    if (last.threads == baseline.threads && last.fds == baseline.fds) {
      break;
    }
    pause_ms(QUIET_STEP_MS);
  }

  bool stopped_cleanly = kvs_test_server_stop(&server);

  bool passed = true;
  char reasons[4][ERROR_CAP];
  int reason_count = 0;
  if (!sampled || started != CLIENTS || warm_rss < 0) {
    (void)snprintf(reasons[reason_count++], ERROR_CAP, "the soak could not run to the end");
  }
  for (int i = 0; i < started; i++) {
    if (clients[i].error[0] != '\0') {
      printf("---- soak: client %d stopped: %s\n", i, clients[i].error);
      passed = false;
    }
  }
  if (sampled && last.fds != baseline.fds) {
    (void)snprintf(reasons[reason_count++], ERROR_CAP, "descriptors went from %d to %d",
                   baseline.fds, last.fds);
  }
  if (sampled && last.threads != baseline.threads) {
    (void)snprintf(reasons[reason_count++], ERROR_CAP, "threads went from %d to %d",
                   baseline.threads, last.threads);
  }
  if (sampled && warm_rss >= 0 && last.rss_kib > warm_rss + (warm_rss * RSS_SLACK_PERCENT / 100)) {
    (void)snprintf(reasons[reason_count++], ERROR_CAP,
                   "resident memory grew from %ld KiB after warm-up to %ld KiB", warm_rss,
                   last.rss_kib);
  }
  passed = passed && reason_count == 0 && stopped_cleanly;

  if (!passed) {
    print_samples(&baseline, samples, n, &last);
    for (int i = 0; i < reason_count; i++) {
      printf("---- soak: %s\n", reasons[i]);
    }
    if (!stopped_cleanly) {
      printf("---- soak: the server did not stop cleanly\n");
    }
    (void)fflush(stdout);
    kvs_test_server_dump(&server);
  }
  return passed ? 0 : 1;
}
