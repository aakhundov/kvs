#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <utest.h>

#include "logger.h"

#include "support/logger.h"

// The logger in-process, through the logging calls themselves: each
// scenario runs in a child of its own (the harness forks), logs through
// KVS_PRINT and KVS_PRINT_WITH_ID, and stops the logger; the parent then
// checks every line the child's standard error carried. A scenario's own
// checks (what attach returned) come back as its exit status.

#define SEQ_TOPIC "seq"
#define MAX_IDS 8
#define MANY_LINES 1000 // far more than one ring holds
#define LONG_MESSAGE 1000

// too large for the stack frame utest puts the fixture in; the tests run
// one at a time, so they share it
static kvs_test_log_t captured;

struct logger {
  kvs_test_log_t *log;
};

UTEST_F_SETUP(logger) {
  utest_fixture->log = &captured;
}

// NOLINTNEXTLINE(readability-non-const-parameter): utest's signature
UTEST_F_TEARDOWN(logger) {
  if (*utest_result != UTEST_TEST_PASSED) {
    kvs_test_log_dump(utest_fixture->log);
  }
}

#define RUN(scenario, arg) ASSERT_TRUE(kvs_test_log_run(utest_fixture->log, (scenario), (arg)))

// A writer logs (lines) numbered lines from (first) under its (id), each
// "---- [id] seq: n", holding a ring while it does if it can get one.
typedef struct writer_t {
  pthread_t thread;
  int id;
  int first;
  int lines;
  bool attached; // what attach returned
} writer_t;

static void *write_lines(void *arg) {
  writer_t *w = arg;
  w->attached = kvs_logger_async_attach();
  for (int i = w->first; i < w->first + w->lines; i++) {
    KVS_PRINT_WITH_ID(SEQ_TOPIC, w->id, "%d", i);
  }
  kvs_logger_async_detach();
  return NULL;
}

static bool run_writer(writer_t *w) {
  return pthread_create(&w->thread, NULL, write_lines, w) == 0 &&
         pthread_join(w->thread, NULL) == 0;
}

// true if every line of (text) is "---- [id] seq: n" with id below (ids),
// and each id's numbers run 0, 1, 2, ... with none missing or reordered.
// (counts) gets how many lines each id wrote.
static bool in_order(const char *text, int ids, long *counts) {
  static const char prefix[] = "---- [";
  static const char middle[] = "] " SEQ_TOPIC ": ";

  for (int id = 0; id < ids; id++) {
    counts[id] = 0;
  }
  const char *p = text;
  while (*p != '\0') {
    if (strncmp(p, prefix, sizeof(prefix) - 1) != 0) {
      return false;
    }
    p += sizeof(prefix) - 1;

    char *end = NULL;
    long id = strtol(p, &end, 10);
    if (end == p || id < 0 || id >= ids || strncmp(end, middle, sizeof(middle) - 1) != 0) {
      return false;
    }
    p = end + sizeof(middle) - 1;

    long n = strtol(p, &end, 10);
    if (end == p || *end != '\n' || n != counts[id]) {
      return false;
    }
    counts[id]++;
    p = end + 1;
  }
  return true;
}

// ---- lines written straight to standard error

static int one_line_without_an_id(void *arg) {
  (void)arg;
  if (!kvs_logger_init(1)) {
    return kvs_test_log_fail("init failed");
  }
  KVS_PRINT("topic", "hello %d", 42);
  kvs_logger_free(true);
  return 0;
}

UTEST_F(logger, a_line_without_an_id_is_written_with_its_topic) {
  RUN(one_line_without_an_id, NULL);
  EXPECT_STREQ("---- topic: hello 42\n", utest_fixture->log->text);
}

static int one_line_with_an_id(void *arg) {
  (void)arg;
  if (!kvs_logger_init(1)) {
    return kvs_test_log_fail("init failed");
  }
  KVS_PRINT_WITH_ID("topic", 7, "hello %s", "there");
  kvs_logger_free(true);
  return 0;
}

UTEST_F(logger, a_line_with_an_id_is_written_with_the_id_and_its_topic) {
  RUN(one_line_with_an_id, NULL);
  EXPECT_STREQ("---- [7] topic: hello there\n", utest_fixture->log->text);
}

static int no_rings(void *arg) {
  (void)arg;
  if (!kvs_logger_init(0)) {
    return kvs_test_log_fail("init with no rings failed");
  }
  if (kvs_logger_async_attach()) {
    return kvs_test_log_fail("attach succeeded with no rings");
  }
  KVS_PRINT("topic", "direct");
  kvs_logger_async_detach();
  kvs_logger_free(true);
  return 0;
}

UTEST_F(logger, a_pool_of_no_rings_leaves_every_thread_writing_directly) {
  RUN(no_rings, NULL);
  EXPECT_STREQ("---- topic: direct\n", utest_fixture->log->text);
}

// ---- lines handed to the logger through a ring

static int many_lines_from_one_thread(void *arg) {
  (void)arg;
  if (!kvs_logger_init(1)) {
    return kvs_test_log_fail("init failed");
  }
  writer_t w = {.id = 0, .first = 0, .lines = MANY_LINES};
  if (!run_writer(&w)) {
    return kvs_test_log_fail("writer thread failed");
  }
  kvs_logger_free(true);
  return w.attached ? 0 : kvs_test_log_fail("attach failed");
}

UTEST_F(logger, every_line_through_a_full_ring_arrives_in_order) {
  RUN(many_lines_from_one_thread, NULL);
  long counts[1];
  ASSERT_TRUE(in_order(utest_fixture->log->text, 1, counts));
  EXPECT_EQ(MANY_LINES, counts[0]);
}

#define WRITERS 4

static int many_lines_from_concurrent_threads(void *arg) {
  (void)arg;
  if (!kvs_logger_init(WRITERS)) {
    return kvs_test_log_fail("init failed");
  }
  writer_t w[WRITERS];
  for (int i = 0; i < WRITERS; i++) {
    w[i] = (writer_t){.id = i, .first = 0, .lines = MANY_LINES};
    if (pthread_create(&w[i].thread, NULL, write_lines, &w[i]) != 0) {
      return kvs_test_log_fail("writer thread failed");
    }
  }
  for (int i = 0; i < WRITERS; i++) {
    pthread_join(w[i].thread, NULL);
  }
  kvs_logger_free(true);
  for (int i = 0; i < WRITERS; i++) {
    if (!w[i].attached) {
      return kvs_test_log_fail("attach failed");
    }
  }
  return 0;
}

UTEST_F(logger, concurrent_threads_each_keep_every_line_in_order) {
  RUN(many_lines_from_concurrent_threads, NULL);
  long counts[WRITERS];
  ASSERT_TRUE(in_order(utest_fixture->log->text, WRITERS, counts));
  for (int i = 0; i < WRITERS; i++) {
    EXPECT_EQ(MANY_LINES, counts[i]);
  }
}

static int two_owners_of_one_ring(void *arg) {
  (void)arg;
  if (!kvs_logger_init(1)) {
    return kvs_test_log_fail("init failed");
  }
  // the second owner continues the first one's numbering under the same
  // id: the lines run 0 .. 199 in order only if the first owner's come first
  writer_t first = {.id = 0, .first = 0, .lines = 100};
  writer_t second = {.id = 0, .first = 100, .lines = 100};
  if (!run_writer(&first) || !run_writer(&second)) {
    return kvs_test_log_fail("writer thread failed");
  }
  kvs_logger_free(true);
  if (!first.attached || !second.attached) {
    return kvs_test_log_fail("attach failed");
  }
  return 0;
}

UTEST_F(logger, a_ring_given_back_keeps_the_previous_owner_s_lines_first) {
  RUN(two_owners_of_one_ring, NULL);
  long counts[1];
  ASSERT_TRUE(in_order(utest_fixture->log->text, 1, counts));
  EXPECT_EQ(200, counts[0]);
}

static int long_message_on_both_paths(void *arg) {
  (void)arg;
  if (!kvs_logger_init(1)) {
    return kvs_test_log_fail("init failed");
  }
  char message[LONG_MESSAGE + 1];
  memset(message, 'x', LONG_MESSAGE);
  message[LONG_MESSAGE] = '\0';

  KVS_PRINT("t", "%s", message); // no ring: written directly
  if (!kvs_logger_async_attach()) {
    return kvs_test_log_fail("attach failed");
  }
  KVS_PRINT("t", "%s", message); // through the ring
  kvs_logger_async_detach();
  kvs_logger_free(true);
  return 0;
}

UTEST_F(logger, a_long_message_is_cut_to_an_ellipsis_the_same_way_on_both_paths) {
  RUN(long_message_on_both_paths, NULL);
  const char *text = utest_fixture->log->text;
  const char *second = strchr(text, '\n');
  ASSERT_TRUE(second != NULL);
  second++;
  size_t line_len = (size_t)(second - text);

  // the two lines are the same line
  ASSERT_EQ(2 * line_len, strlen(text));
  EXPECT_EQ(0, strncmp(text, second, line_len));

  // "---- t: " + x...x + "..." + "\n", shorter than the message logged
  static const char prefix[] = "---- t: ";
  ASSERT_EQ(0, strncmp(text, prefix, sizeof(prefix) - 1));
  const char *message = text + sizeof(prefix) - 1;
  size_t message_len = line_len - (sizeof(prefix) - 1) - 1;
  ASSERT_LT(message_len, (size_t)LONG_MESSAGE);
  ASSERT_LE((size_t)3, message_len);
  EXPECT_EQ(0, strncmp(message + message_len - 3, "...", 3));
  EXPECT_EQ(message_len - 3, strspn(message, "x"));
}

// ---- taking and giving back rings

static int attach_when_every_ring_is_taken(void *arg) {
  (void)arg;
  if (!kvs_logger_init(1)) {
    return kvs_test_log_fail("init failed");
  }
  if (!kvs_logger_async_attach()) {
    return kvs_test_log_fail("attach to the only ring failed");
  }
  // the only ring is held: this writer's attach fails, and its one line is
  // written directly
  writer_t shut_out = {.id = 1, .first = 0, .lines = 1};
  if (!run_writer(&shut_out)) {
    return kvs_test_log_fail("writer thread failed");
  }
  kvs_logger_async_detach();
  // given back: the next writer gets it
  writer_t next = {.id = 0, .first = 0, .lines = 0};
  if (!run_writer(&next)) {
    return kvs_test_log_fail("writer thread failed");
  }
  kvs_logger_free(true);

  if (shut_out.attached) {
    return kvs_test_log_fail("attach succeeded with every ring taken");
  }
  if (!next.attached) {
    return kvs_test_log_fail("attach failed after the ring was given back");
  }
  return 0;
}

UTEST_F(logger, attach_fails_when_every_ring_is_taken_and_the_thread_writes_directly) {
  RUN(attach_when_every_ring_is_taken, NULL);
  EXPECT_STREQ("---- [1] " SEQ_TOPIC ": 0\n", utest_fixture->log->text);
}

static int attach_and_detach_twice(void *arg) {
  (void)arg;
  if (!kvs_logger_init(1)) {
    return kvs_test_log_fail("init failed");
  }
  if (!kvs_logger_async_attach()) {
    return kvs_test_log_fail("attach failed");
  }
  if (!kvs_logger_async_attach()) {
    return kvs_test_log_fail("second attach failed");
  }
  // a second attach took no second ring: there is none, and the thread
  // still holds the only one
  writer_t other = {.id = 0, .first = 0, .lines = 0};
  if (!run_writer(&other)) {
    return kvs_test_log_fail("writer thread failed");
  }
  if (other.attached) {
    return kvs_test_log_fail("another thread took the ring the first still holds");
  }

  kvs_logger_async_detach();
  kvs_logger_async_detach();
  // the ring was given back once and is free
  if (!run_writer(&other)) {
    return kvs_test_log_fail("writer thread failed");
  }
  kvs_logger_free(true);
  return other.attached ? 0 : kvs_test_log_fail("attach failed after the ring was given back");
}

UTEST_F(logger, attach_and_detach_are_idempotent) {
  RUN(attach_and_detach_twice, NULL);
  EXPECT_STREQ("", utest_fixture->log->text);
}

// ---- stopping

static int log_across_a_stop_that_keeps_the_pool(void *arg) {
  (void)arg;
  if (!kvs_logger_init(1)) {
    return kvs_test_log_fail("init failed");
  }
  if (!kvs_logger_async_attach()) {
    return kvs_test_log_fail("attach failed");
  }
  for (int i = 0; i < 5; i++) {
    KVS_PRINT_WITH_ID(SEQ_TOPIC, 0, "%d", i);
  }
  // a stop that missed its deadline: the logger stops, the pool stays, and
  // this thread, still attached, goes on writing into its ring
  kvs_logger_free(false);
  for (int i = 5; i < 10; i++) {
    KVS_PRINT_WITH_ID(SEQ_TOPIC, 0, "%d", i);
  }
  kvs_logger_async_detach();
  return 0;
}

UTEST_F(logger, a_stop_that_keeps_the_pool_writes_the_lines_before_it_and_no_others) {
  RUN(log_across_a_stop_that_keeps_the_pool, NULL);
  long counts[1];
  ASSERT_TRUE(in_order(utest_fixture->log->text, 1, counts));
  EXPECT_EQ(5, counts[0]);
}

#define STOP_MARK_TOPIC "mark"
// far above two rings' worth of lines (a ring holds 64), far below what a
// logger chasing a writer that never stops writes before it catches up
#define STOP_LINE_BOUND 1000

static atomic_long lines_logged;

static void *log_forever(void *arg) {
  (void)arg;
  if (!kvs_logger_async_attach()) {
    return NULL;
  }
  for (long i = 0;; i++) {
    KVS_PRINT_WITH_ID(SEQ_TOPIC, 0, "%ld", i);
    atomic_fetch_add(&lines_logged, 1);
  }
}

static int stop_while_a_thread_keeps_logging(void *arg) {
  (void)arg;
  if (!kvs_logger_init(1)) {
    return kvs_test_log_fail("init failed");
  }
  pthread_t thread;
  if (pthread_create(&thread, NULL, log_forever, NULL) != 0) {
    return kvs_test_log_fail("writer thread failed");
  }
  struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000L * 1000L};
  while (atomic_load(&lines_logged) < MANY_LINES) {
    nanosleep(&pause, NULL);
  }
  // written directly (this thread holds no ring) just before the stop: the
  // lines after it are the ones the logger wrote while stopping
  KVS_PRINT(STOP_MARK_TOPIC, "stop");
  // the writer never stops, and the stop has to return anyway. it keeps
  // the pool because the writer is still writing into its ring; the
  // process exits with the writer still running.
  kvs_logger_free(false);
  return 0;
}

UTEST_F(logger, the_logger_stops_while_a_thread_keeps_logging) {
  RUN(stop_while_a_thread_keeps_logging, NULL);
  ASSERT_FALSE(utest_fixture->log->truncated);
  const char *mark = strstr(utest_fixture->log->text, "---- " STOP_MARK_TOPIC ": stop\n");
  ASSERT_TRUE(mark != NULL);

  // a stop finishes the walk it is in and makes one more, each writing at
  // most what the ring held: two rings' worth. a logger that chased the
  // writer instead would write for as long as the writer kept up.
  long after = 0;
  for (const char *p = strchr(mark, '\n') + 1; *p != '\0'; p++) {
    after += *p == '\n';
  }
  EXPECT_LE(after, (long)STOP_LINE_BOUND);
}
