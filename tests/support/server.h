#ifndef KVS_TEST_SERVER_H
#define KVS_TEST_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sys/types.h>

// The spawned-server harness of 1e: kvs runs as a child process with its
// standard error on a pipe, the harness reads that pipe for the listening
// line, drives the server over real sockets, stops it with SIGTERM and
// reaps it. Everything the child says after the listening line is kept in
// (said) and printed only when a test fails, so a passing run is silent.
//
// The server binary is named by the KVS_SERVER environment variable, which
// the Makefile's test target sets. The child is always asked for port 0.

#define KVS_TEST_SAID_SIZE 65536
#define KVS_TEST_LINE_SIZE 2048

typedef struct kvs_test_server_t {
  pid_t pid;
  int stderr_fd; // read end of the pipe carrying the child's stderr
  uint16_t port; // parsed from the listening line
  int status;    // as waitpid wrote it, valid once stopped
  bool stopped;
  char said[KVS_TEST_SAID_SIZE];
  size_t said_len;
} kvs_test_server_t;

// spawns the server. returns true once the listening line has been read;
// false if the child died before printing it, in which case the child has
// been reaped and (status) holds its exit status.
bool kvs_test_server_spawn(kvs_test_server_t *server);

// same, with one extra environment variable set for the child.
bool kvs_test_server_spawn_with(kvs_test_server_t *server, const char *name, const char *value);

// connects a client socket to the server, with a receive timeout so that a
// server that stops answering fails the test instead of hanging it. -1 on
// failure.
int kvs_test_server_connect(const kvs_test_server_t *server);

// sends SIGTERM, drains the child's stderr to end of input, reaps the child.
// returns true if the child exited normally with status 0.
bool kvs_test_server_stop(kvs_test_server_t *server);

// prints everything the child said to stderr: the diagnosis, for a failure.
void kvs_test_server_dump(const kvs_test_server_t *server);

// writes all (n) bytes to (fd). true on success.
bool kvs_test_send(int fd, const char *bytes, size_t n);

// reads one reply line from (fd), the line feed stripped, into (buf) as a
// NUL-terminated string. returns the line length, or -1 on end of input,
// timeout or a line that does not fit.
long kvs_test_recv_line(int fd, char *buf, size_t cap);

// reads until end of input, discarding. true if end of input was reached
// (rather than a timeout or an error).
bool kvs_test_recv_eof(int fd);

// sends (request) plus a line feed and reads one reply line into (reply).
// true on success.
bool kvs_test_request(int fd, const char *request, char *reply, size_t cap);

#endif
