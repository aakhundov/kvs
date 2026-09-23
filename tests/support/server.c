#include "server.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h> // NOLINT(misc-include-cleaner): htons, htonl
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h> // NOLINT(misc-include-cleaner): struct timeval
#include <sys/wait.h>
#include <unistd.h>

// Both bounds are hang detectors, not synchronisation: a healthy server
// answers in microseconds and closes its stderr the moment it exits, so a
// test only ever waits this long when something is already wrong.
#define KVS_TEST_RECV_TIMEOUT_MS 2000
#define KVS_TEST_STDERR_TIMEOUT_MS 5000

#define KVS_TEST_LISTENING_MARK "listening "

static void say(kvs_test_server_t *server, const char *text, size_t n) {
  size_t room = sizeof(server->said) - server->said_len - 1;
  if (n > room) {
    n = room;
  }
  memcpy(server->said + server->said_len, text, n);
  server->said_len += n;
  server->said[server->said_len] = '\0';
}

// reads one line of the child's stderr into (line) and appends it to
// (said). returns false on end of input, error, or a child that has gone
// quiet for longer than the bound (which is then killed, so that the
// suite ends instead of hanging).
static bool read_said_line(kvs_test_server_t *server, char *line, size_t cap) {
  size_t used = 0;
  while (used + 1 < cap) {
    struct pollfd pfd = {.fd = server->stderr_fd, .events = POLLIN};
    int ready = poll(&pfd, 1, KVS_TEST_STDERR_TIMEOUT_MS);
    if (ready < 0 && errno == EINTR) {
      continue;
    }
    if (ready == 0) {
      say(server, "[harness] child went quiet: sending SIGKILL\n", 43);
      kill(server->pid, SIGKILL);
      return false;
    }

    char c;
    ssize_t n = read(server->stderr_fd, &c, 1);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      return false;
    }
    say(server, &c, 1);
    if (c == '\n') {
      break;
    }
    line[used++] = c;
  }
  line[used] = '\0';
  return true;
}

static void reap(kvs_test_server_t *server) {
  int status = 0;
  while (waitpid(server->pid, &status, 0) < 0 && errno == EINTR) {
  }
  server->status = status;
  server->stopped = true;
  close(server->stderr_fd);
  server->stderr_fd = -1;
}

bool kvs_test_server_spawn_with(kvs_test_server_t *server, const char *name, const char *value) {
  memset(server, 0, sizeof *server);
  server->pid = -1;
  server->stderr_fd = -1;

  // copied: the child's setenv calls may invalidate what getenv returned
  const char *configured = getenv("KVS_SERVER");
  if (configured == NULL) {
    say(server, "[harness] KVS_SERVER is not set: run the suite through make test\n", 66);
    server->stopped = true;
    return false;
  }
  char path[KVS_TEST_LINE_SIZE];
  snprintf(path, sizeof path, "%s", configured);

  int pipe_fds[2];
  if (pipe(pipe_fds) != 0) {
    say(server, "[harness] pipe failed\n", 22);
    server->stopped = true;
    return false;
  }

  pid_t kid = fork();
  if (kid < 0) {
    say(server, "[harness] fork failed\n", 22);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    server->stopped = true;
    return false;
  }
  if (kid == 0) {
    close(pipe_fds[0]);
    dup2(pipe_fds[1], STDERR_FILENO);
    close(pipe_fds[1]);
    setenv("KVS_PORT", "0", 1);
    if (name != NULL) {
      setenv(name, value, 1);
    }
    // the path is the Makefile's, not a client's: no sanitising to do
    execl(path, "kvs", (char *)NULL); // NOLINT(clang-analyzer-optin.taint.GenericTaint)
    _exit(127);
  }
  close(pipe_fds[1]);
  server->pid = kid;
  server->stderr_fd = pipe_fds[0];

  char line[KVS_TEST_LINE_SIZE];
  while (read_said_line(server, line, sizeof line)) {
    const char *mark = strstr(line, KVS_TEST_LISTENING_MARK);
    if (mark == NULL) {
      continue;
    }
    // "listening <address> <port>": the port is the text after the space
    const char *text = strchr(mark + strlen(KVS_TEST_LISTENING_MARK), ' ');
    if (text == NULL) {
      continue;
    }
    errno = 0;
    char *end = NULL;
    unsigned long port = strtoul(text + 1, &end, 10);
    if (errno == 0 && end != text + 1 && *end == '\0' && port <= UINT16_MAX) {
      server->port = (uint16_t)port;
      return true;
    }
  }

  // no listening line: the child is gone, or was killed for going quiet
  reap(server);
  return false;
}

bool kvs_test_server_spawn(kvs_test_server_t *server) {
  return kvs_test_server_spawn_with(server, NULL, NULL);
}

int kvs_test_server_connect(const kvs_test_server_t *server) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }

  struct timeval tv = {.tv_sec = KVS_TEST_RECV_TIMEOUT_MS / 1000,
                       .tv_usec = (KVS_TEST_RECV_TIMEOUT_MS % 1000) * 1000L};
  if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) != 0) {
    close(fd);
    return -1;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(server->port);
  if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

bool kvs_test_server_stop(kvs_test_server_t *server) {
  if (!server->stopped) {
    kill(server->pid, SIGTERM);
    char line[KVS_TEST_LINE_SIZE];
    while (read_said_line(server, line, sizeof line)) {
    }
    reap(server);
  }
  return WIFEXITED(server->status) && WEXITSTATUS(server->status) == 0;
}

void kvs_test_server_dump(const kvs_test_server_t *server) {
  fputs("---- the server said:\n", stderr);
  fputs(server->said, stderr);
  if (server->stopped) {
    if (WIFEXITED(server->status)) {
      fprintf(stderr, "---- the server exited with status %d\n", WEXITSTATUS(server->status));
    } else if (WIFSIGNALED(server->status)) {
      fprintf(stderr, "---- the server was killed by signal %d\n", WTERMSIG(server->status));
    }
  }
}

bool kvs_test_send(int fd, const char *bytes, size_t n) {
  while (n > 0) {
    ssize_t written = write(fd, bytes, n);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      return false;
    }
    bytes += written;
    n -= (size_t)written;
  }
  return true;
}

long kvs_test_recv_line(int fd, char *buf, size_t cap) {
  size_t used = 0;
  while (used + 1 < cap) {
    char c;
    ssize_t n = read(fd, &c, 1);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      return -1;
    }
    if (c == '\n') {
      buf[used] = '\0';
      return (long)used;
    }
    buf[used++] = c;
  }
  return -1;
}

bool kvs_test_recv_eof(int fd) {
  char buf[256];
  while (true) {
    ssize_t n = read(fd, buf, sizeof buf);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0) {
      return false;
    }
    if (n == 0) {
      return true;
    }
  }
}

bool kvs_test_request(int fd, const char *request, char *reply, size_t cap) {
  // one write for the whole line: a separate write for the line feed
  // waits behind the delayed ack of the first under Nagle's algorithm
  char line[KVS_TEST_LINE_SIZE];
  int n = snprintf(line, sizeof line, "%s\n", request);
  if (n < 0 || (size_t)n >= sizeof line) {
    return false;
  }
  return kvs_test_send(fd, line, (size_t)n) && kvs_test_recv_line(fd, reply, cap) >= 0;
}
