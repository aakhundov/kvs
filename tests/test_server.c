#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dirent.h>
#include <sys/socket.h>
#include <sys/wait.h> // NOLINT(misc-include-cleaner): WIFEXITED, WEXITSTATUS
#include <unistd.h>

#include <utest.h>

#include "server.h"

#include "support/server.h"

// kvs end to end, as 1e's second process: spawned, driven over a real
// socket, stopped with SIGTERM and reaped. Each test gets a fresh server;
// the teardown stops it and requires a clean exit, and prints what the
// server said only when something failed.

#define REPLY_CAP (KVS_MAX_LINE_LENGTH + 1)

struct server {
  kvs_test_server_t server;
  int client;
};

UTEST_F_SETUP(server) {
  utest_fixture->client = -1;
  if (!kvs_test_server_spawn(&utest_fixture->server)) {
    kvs_test_server_dump(&utest_fixture->server);
    ASSERT_TRUE(false);
  }
  utest_fixture->client = kvs_test_server_connect(&utest_fixture->server);
  ASSERT_LE(0, utest_fixture->client);
}

UTEST_F_TEARDOWN(server) {
  if (utest_fixture->client >= 0) {
    close(utest_fixture->client);
  }
  bool clean = kvs_test_server_stop(&utest_fixture->server);
  if (!clean || *utest_result != UTEST_TEST_PASSED) {
    kvs_test_server_dump(&utest_fixture->server);
  }
  EXPECT_TRUE(clean);
}

#define EXPECT_REPLY(fd, request, expected)                                                        \
  do {                                                                                             \
    char reply_[REPLY_CAP];                                                                        \
    ASSERT_TRUE(kvs_test_request((fd), (request), reply_, sizeof reply_));                         \
    EXPECT_STREQ((expected), reply_);                                                              \
  } while (0)

#define EXPECT_ERR(fd, request)                                                                    \
  do {                                                                                             \
    char reply_[REPLY_CAP];                                                                        \
    ASSERT_TRUE(kvs_test_request((fd), (request), reply_, sizeof reply_));                         \
    EXPECT_EQ(0, strncmp("ERR ", reply_, 4));                                                      \
  } while (0)

// the next reply line, for requests sent by other means
#define EXPECT_NEXT_LINE(fd, expected)                                                             \
  do {                                                                                             \
    char reply_[REPLY_CAP];                                                                        \
    ASSERT_LE(0, kvs_test_recv_line((fd), reply_, sizeof reply_));                                 \
    EXPECT_STREQ((expected), reply_);                                                              \
  } while (0)

UTEST_F(server, starts_on_a_kernel_chosen_port_and_stops_cleanly) {
  EXPECT_NE(0, utest_fixture->server.port);
}

UTEST_F(server, answers_set_get_del) {
  int fd = utest_fixture->client;
  EXPECT_REPLY(fd, "SET alpha 42", "OK");
  EXPECT_REPLY(fd, "GET alpha", "VAL 42");
  EXPECT_REPLY(fd, "GET missing", "NIL");
  EXPECT_REPLY(fd, "DEL alpha", "OK");
  EXPECT_REPLY(fd, "DEL alpha", "NIL");
  EXPECT_REPLY(fd, "GET alpha", "NIL");
}

UTEST_F(server, several_requests_in_one_write_get_their_replies_in_order) {
  int fd = utest_fixture->client;
  const char *batch = "SET a 1\nSET b 2\nGET a\nGET b\nDEL a\nGET a\n";
  ASSERT_TRUE(kvs_test_send(fd, batch, strlen(batch)));
  EXPECT_NEXT_LINE(fd, "OK");
  EXPECT_NEXT_LINE(fd, "OK");
  EXPECT_NEXT_LINE(fd, "VAL 1");
  EXPECT_NEXT_LINE(fd, "VAL 2");
  EXPECT_NEXT_LINE(fd, "OK");
  EXPECT_NEXT_LINE(fd, "NIL");
}

UTEST_F(server, a_request_sent_in_pieces_is_one_request) {
  int fd = utest_fixture->client;
  ASSERT_TRUE(kvs_test_send(fd, "SET al", 6));
  ASSERT_TRUE(kvs_test_send(fd, "pha 4", 5));
  ASSERT_TRUE(kvs_test_send(fd, "2\n", 2));
  EXPECT_NEXT_LINE(fd, "OK");
  EXPECT_REPLY(fd, "GET alpha", "VAL 42");
}

UTEST_F(server, crlf_endings_are_the_same_request) {
  int fd = utest_fixture->client;
  ASSERT_TRUE(kvs_test_send(fd, "SET a 1\r\n", 9));
  EXPECT_NEXT_LINE(fd, "OK");
  ASSERT_TRUE(kvs_test_send(fd, "GET a\r\r\n", 8));
  EXPECT_NEXT_LINE(fd, "VAL 1");
  EXPECT_REPLY(fd, "GET a", "VAL 1");
}

UTEST_F(server, replies_end_with_a_line_feed_and_no_carriage_return) {
  int fd = utest_fixture->client;
  ASSERT_TRUE(kvs_test_send(fd, "GET a\r\n", 7));
  char raw[4];
  size_t got = 0;
  while (got < sizeof raw) {
    ssize_t n = read(fd, raw + got, sizeof raw - got);
    ASSERT_LT((ssize_t)0, n);
    got += (size_t)n;
  }
  EXPECT_EQ(0, memcmp("NIL\n", raw, 4));
}

UTEST_F(server, an_empty_request_gets_an_error_and_the_connection_carries_on) {
  int fd = utest_fixture->client;
  EXPECT_ERR(fd, "");
  EXPECT_REPLY(fd, "SET a 1", "OK");
}

UTEST_F(server, malformed_requests_get_an_error_each) {
  int fd = utest_fixture->client;
  EXPECT_ERR(fd, "PUT a 1");
  EXPECT_ERR(fd, "get a");
  EXPECT_ERR(fd, "GET");
  EXPECT_ERR(fd, "GET ");
  EXPECT_ERR(fd, "GET a b");
  EXPECT_ERR(fd, "SET a");
  EXPECT_ERR(fd, " SET a 1");
  EXPECT_REPLY(fd, "GET a", "NIL");
}

UTEST_F(server, a_carriage_return_inside_a_request_is_an_error) {
  int fd = utest_fixture->client;
  EXPECT_ERR(fd, "SET a\rb 1");
  EXPECT_ERR(fd, "GET a\rb");
  EXPECT_ERR(fd, "SET a 1\r2");
  EXPECT_REPLY(fd, "GET a", "NIL");
}

UTEST_F(server, a_nul_byte_inside_a_request_is_an_error) {
  int fd = utest_fixture->client;
  ASSERT_TRUE(kvs_test_send(fd, "SET a\0b 1\n", 10));
  char reply[REPLY_CAP];
  ASSERT_LE(0, kvs_test_recv_line(fd, reply, sizeof reply));
  EXPECT_EQ(0, strncmp("ERR ", reply, 4));
  EXPECT_REPLY(fd, "GET a", "NIL");
}

UTEST_F(server, a_tab_is_data) {
  int fd = utest_fixture->client;
  EXPECT_REPLY(fd, "SET a\tb 1", "OK");
  EXPECT_REPLY(fd, "GET a\tb", "VAL 1");
}

UTEST_F(server, a_request_at_the_bound_is_accepted) {
  int fd = utest_fixture->client;
  char request[KVS_MAX_LINE_LENGTH + 1];
  memcpy(request, "SET k ", 6);
  memset(request + 6, 'v', KVS_MAX_LINE_LENGTH - 6);
  request[KVS_MAX_LINE_LENGTH] = '\0';
  EXPECT_REPLY(fd, request, "OK");

  char reply[REPLY_CAP];
  ASSERT_TRUE(kvs_test_request(fd, "GET k", reply, sizeof reply));
  ASSERT_EQ((size_t)(KVS_MAX_LINE_LENGTH - 2), strlen(reply));
  EXPECT_EQ(0, strncmp("VAL ", reply, 4));
  EXPECT_EQ(0, strcmp(request + 6, reply + 4));
}

UTEST_F(server, a_request_over_the_bound_gets_line_too_long_and_the_next_one_is_served) {
  int fd = utest_fixture->client;
  char request[KVS_MAX_LINE_LENGTH + 2];
  memcpy(request, "SET k ", 6);
  memset(request + 6, 'v', KVS_MAX_LINE_LENGTH - 5);
  request[KVS_MAX_LINE_LENGTH + 1] = '\0';
  EXPECT_REPLY(fd, request, "ERR line too long");
  EXPECT_REPLY(fd, "GET k", "NIL");
  EXPECT_REPLY(fd, "SET k 1", "OK");
  EXPECT_REPLY(fd, "GET k", "VAL 1");
}

UTEST_F(server, a_request_far_over_the_bound_is_discarded_through_its_line_feed) {
  int fd = utest_fixture->client;
  char big[20000];
  memset(big, 'x', sizeof big);
  big[sizeof big - 1] = '\n';
  ASSERT_TRUE(kvs_test_send(fd, big, sizeof big));
  EXPECT_NEXT_LINE(fd, "ERR line too long");
  EXPECT_REPLY(fd, "GET x", "NIL");
}

UTEST_F(server, trailing_carriage_returns_do_not_count_against_the_bound) {
  int fd = utest_fixture->client;
  char request[KVS_MAX_LINE_LENGTH + 4];
  snprintf(request, sizeof request, "SET k ");
  memset(request + 6, 'v', KVS_MAX_LINE_LENGTH - 6);
  request[KVS_MAX_LINE_LENGTH] = '\r';
  request[KVS_MAX_LINE_LENGTH + 1] = '\r';
  request[KVS_MAX_LINE_LENGTH + 2] = '\n';
  ASSERT_TRUE(kvs_test_send(fd, request, KVS_MAX_LINE_LENGTH + 3));
  EXPECT_NEXT_LINE(fd, "OK");
}

UTEST_F(server, the_store_is_shared_across_connections) {
  EXPECT_REPLY(utest_fixture->client, "SET shared 1", "OK");
  close(utest_fixture->client);
  utest_fixture->client = kvs_test_server_connect(&utest_fixture->server);
  ASSERT_LE(0, utest_fixture->client);
  EXPECT_REPLY(utest_fixture->client, "GET shared", "VAL 1");
}

UTEST_F(server, a_client_that_leaves_mid_request_does_not_take_the_server_down) {
  ASSERT_TRUE(kvs_test_send(utest_fixture->client, "SET a", 5));
  close(utest_fixture->client);
  utest_fixture->client = kvs_test_server_connect(&utest_fixture->server);
  ASSERT_LE(0, utest_fixture->client);
  EXPECT_REPLY(utest_fixture->client, "GET a", "NIL");
  EXPECT_REPLY(utest_fixture->client, "SET a 1", "OK");
}

UTEST_F(server, a_partial_final_request_gets_no_reply) {
  int fd = utest_fixture->client;
  ASSERT_TRUE(kvs_test_send(fd, "SET a 1\nGET a", 13));
  ASSERT_EQ(0, shutdown(fd, SHUT_WR));
  EXPECT_NEXT_LINE(fd, "OK");
  EXPECT_TRUE(kvs_test_recv_eof(fd));
}

UTEST_F(server, an_over_long_request_cut_off_by_end_of_input_gets_no_reply) {
  int fd = utest_fixture->client;
  char big[KVS_MAX_LINE_LENGTH + 100];
  memset(big, 'x', sizeof big);
  ASSERT_TRUE(kvs_test_send(fd, big, sizeof big));
  ASSERT_EQ(0, shutdown(fd, SHUT_WR));
  EXPECT_TRUE(kvs_test_recv_eof(fd));
}

UTEST_F(server, no_request_ends_the_connection) {
  int fd = utest_fixture->client;
  EXPECT_ERR(fd, "QUIT");
  EXPECT_ERR(fd, "EXIT");
  EXPECT_REPLY(fd, "GET a", "NIL");
}

// C3: every connection is served on its own thread, so a second client is
// answered while the first is still connected, an idle client holds up
// nobody, and requests interleave across connections against one store.

UTEST_F(server, a_second_connection_is_served_while_the_first_stays_open) {
  int second = kvs_test_server_connect(&utest_fixture->server);
  ASSERT_LE(0, second);
  EXPECT_REPLY(utest_fixture->client, "SET a 1", "OK");
  EXPECT_REPLY(second, "GET a", "VAL 1");
  EXPECT_REPLY(utest_fixture->client, "GET a", "VAL 1");
  close(second);
}

UTEST_F(server, an_idle_client_holds_up_no_other_client) {
  // the fixture's client is connected and sends nothing
  int second = kvs_test_server_connect(&utest_fixture->server);
  ASSERT_LE(0, second);
  EXPECT_REPLY(second, "SET a 1", "OK");
  EXPECT_REPLY(second, "GET a", "VAL 1");
  EXPECT_REPLY(second, "DEL a", "OK");
  close(second);
  EXPECT_REPLY(utest_fixture->client, "GET a", "NIL");
}

UTEST_F(server, requests_interleave_across_connections_against_one_store) {
  int a = utest_fixture->client;
  int b = kvs_test_server_connect(&utest_fixture->server);
  ASSERT_LE(0, b);
  EXPECT_REPLY(a, "SET x 1", "OK");
  EXPECT_REPLY(b, "GET x", "VAL 1");
  EXPECT_REPLY(b, "SET x 2", "OK");
  EXPECT_REPLY(a, "GET x", "VAL 2");
  EXPECT_REPLY(a, "DEL x", "OK");
  EXPECT_REPLY(b, "GET x", "NIL");
  EXPECT_REPLY(b, "DEL x", "NIL");
  close(b);
}

UTEST_F(server, a_client_that_leaves_does_not_disturb_another_open_connection) {
  int second = kvs_test_server_connect(&utest_fixture->server);
  ASSERT_LE(0, second);
  EXPECT_REPLY(second, "SET a 1", "OK");
  ASSERT_TRUE(kvs_test_send(second, "SET b", 5)); // leaves mid-request
  close(second);
  EXPECT_REPLY(utest_fixture->client, "GET a", "VAL 1");
  EXPECT_REPLY(utest_fixture->client, "GET b", "NIL");
}

#define MANY_CONNECTIONS 32

UTEST_F(server, many_connections_open_at_once_are_all_served) {
  int fds[MANY_CONNECTIONS];
  for (int i = 0; i < MANY_CONNECTIONS; i++) {
    fds[i] = kvs_test_server_connect(&utest_fixture->server);
    ASSERT_LE(0, fds[i]);
  }
  char request[32];
  char expected[32];
  for (int i = 0; i < MANY_CONNECTIONS; i++) {
    snprintf(request, sizeof request, "SET key%d %d", i, i);
    EXPECT_REPLY(fds[i], request, "OK");
  }
  for (int i = 0; i < MANY_CONNECTIONS; i++) {
    // each connection reads a key another connection wrote
    int j = (i + 1) % MANY_CONNECTIONS;
    snprintf(request, sizeof request, "GET key%d", j);
    snprintf(expected, sizeof expected, "VAL %d", j);
    EXPECT_REPLY(fds[i], request, expected);
  }
  for (int i = 0; i < MANY_CONNECTIONS; i++) {
    close(fds[i]);
  }
}

#define BURST_WRITERS 8
#define BURST_REQUESTS 64

UTEST_F(server, a_burst_of_writers_to_one_key_gets_every_reply_and_one_final_value) {
  // every writer sends its requests in one write, so the server's threads
  // run the store operations as concurrently as the suite can make them
  int fds[BURST_WRITERS];
  for (int w = 0; w < BURST_WRITERS; w++) {
    fds[w] = kvs_test_server_connect(&utest_fixture->server);
    ASSERT_LE(0, fds[w]);
  }
  for (int w = 0; w < BURST_WRITERS; w++) {
    char batch[BURST_REQUESTS * 16];
    size_t len = 0;
    for (int r = 0; r < BURST_REQUESTS; r++) {
      len += (size_t)snprintf(batch + len, sizeof batch - len, "SET hot %d\n", (w * 1000) + r);
    }
    ASSERT_TRUE(kvs_test_send(fds[w], batch, len));
  }
  for (int w = 0; w < BURST_WRITERS; w++) {
    for (int r = 0; r < BURST_REQUESTS; r++) {
      EXPECT_NEXT_LINE(fds[w], "OK");
    }
  }
  // the final value is the last write of one of the writers, whichever it was
  char reply[REPLY_CAP];
  ASSERT_TRUE(kvs_test_request(utest_fixture->client, "GET hot", reply, sizeof reply));
  ASSERT_EQ(0, strncmp("VAL ", reply, 4));
  int value = (int)strtol(reply + 4, NULL, 10);
  EXPECT_EQ(BURST_REQUESTS - 1, value % 1000);
  EXPECT_LT(value / 1000, BURST_WRITERS);
  for (int w = 0; w < BURST_WRITERS; w++) {
    close(fds[w]);
  }
}

// reads the names of the server's threads from /proc, one per task
static int read_thread_names(pid_t pid, char names[][32], int cap) {
  char path[64];
  snprintf(path, sizeof path, "/proc/%d/task", (int)pid);
  DIR *dir = opendir(path);
  if (dir == NULL) {
    return -1;
  }
  int n = 0;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL && n < cap) {
    if (entry->d_name[0] == '.') {
      continue;
    }
    snprintf(path, sizeof path, "/proc/%d/task/%s/comm", (int)pid, entry->d_name);
    FILE *comm = fopen(path, "r");
    if (comm == NULL) {
      continue;
    }
    if (fgets(names[n], 32, comm) != NULL) {
      names[n][strcspn(names[n], "\n")] = '\0';
      n++;
    }
    fclose(comm);
  }
  closedir(dir);
  return n;
}

UTEST_F(server, every_connection_thread_has_a_distinct_name) {
  int second = kvs_test_server_connect(&utest_fixture->server);
  ASSERT_LE(0, second);
  // a reply proves each connection thread has started and named itself
  EXPECT_REPLY(utest_fixture->client, "GET a", "NIL");
  EXPECT_REPLY(second, "GET a", "NIL");

  // the main thread carries the program's name, and so does a sanitizer's
  // background thread when there is one; the connection threads are the rest
  char names[8][32];
  int n = read_thread_names(utest_fixture->server.pid, names, 8);
  ASSERT_LE(3, n);
  int connection_threads = 0;
  const char *first_name = NULL;
  for (int i = 0; i < n; i++) {
    if (strcmp(names[i], "kvs") == 0) {
      continue;
    }
    connection_threads++;
    if (first_name == NULL) {
      first_name = names[i];
    } else {
      EXPECT_STRNE(first_name, names[i]);
    }
  }
  EXPECT_EQ(2, connection_threads);
  close(second);
}

UTEST_F(server, a_stop_with_two_clients_connected_ends_both_cleanly) {
  int second = kvs_test_server_connect(&utest_fixture->server);
  ASSERT_LE(0, second);
  EXPECT_REPLY(utest_fixture->client, "SET a 1", "OK");
  EXPECT_REPLY(second, "GET a", "VAL 1");
  ASSERT_TRUE(kvs_test_server_stop(&utest_fixture->server));
  EXPECT_TRUE(kvs_test_recv_eof(utest_fixture->client));
  EXPECT_TRUE(kvs_test_recv_eof(second));
  close(second);
}

UTEST_F(server, a_stop_while_a_client_is_connected_ends_the_connection_cleanly) {
  int fd = utest_fixture->client;
  EXPECT_REPLY(fd, "SET a 1", "OK");
  ASSERT_TRUE(kvs_test_server_stop(&utest_fixture->server));
  EXPECT_TRUE(kvs_test_recv_eof(fd));
}

// no fixture: the server is expected to fail to start
UTEST(server_startup, a_bad_address_is_a_fatal_error_with_a_non_zero_status) {
  kvs_test_server_t server;
  ASSERT_FALSE(kvs_test_server_spawn_with(&server, "KVS_ADDRESS", "300.1.1.1"));
  ASSERT_TRUE(server.stopped);
  ASSERT_TRUE(WIFEXITED(server.status));
  EXPECT_NE(0, WEXITSTATUS(server.status));
  EXPECT_TRUE(strstr(server.said, "start failed") != NULL);
}
