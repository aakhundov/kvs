#include "server.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "debug.h"
#include "error.h"
#include "lines.h"

#define LOG_ERROR(function, ...) KVS_LOG(function, __VA_ARGS__)
#define LOG_ERRNO(function) KVS_LOG(function, "%s (%d)", strerror(errno), errno)

#define LOG_LISTENER(...) KVS_LOG_WITH_ID("listener", listener, __VA_ARGS__)
#define LOG_SOCKET(...) KVS_LOG_WITH_ID("socket", socket, __VA_ARGS__)
#define TRACE_LISTENER(...) KVS_TRACE_WITH_ID("listener", listener, __VA_ARGS__)
#define TRACE_SOCKET(...) KVS_TRACE_WITH_ID("socket", socket, __VA_ARGS__)

static volatile sig_atomic_t interrupted = 0;

static void on_interrupt(int sig) {
  (void)sig;
  interrupted = 1;
}

static void setup_signals(void) {
  // disable SIGPIPE handler
  (void)signal(SIGPIPE, SIG_IGN);

  // subscribe to interrupt signals
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sigemptyset(&sa.sa_mask);
  sa.sa_handler = on_interrupt;
  sa.sa_flags = 0; // no SA_RESTART: not resumed
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
}

static int make_listener(const char *address, uint16_t *port) {
  struct in_addr sin_addr;
  int result = inet_pton(AF_INET, address, &sin_addr);
  if (result == -1) {
    LOG_ERRNO("inet_pton");
    return -1;
  }
  if (result == 0) {
    LOG_ERROR("inet_pton", "bad address '%s'", address);
    return -1;
  }

  int listener = socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0) {
    LOG_ERRNO("socket");
    return -1;
  }

  int on = 1;
  if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) != 0) {
    LOG_ERRNO("setsockopt");
    close(listener);
    return -1;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr = sin_addr;
  addr.sin_port = htons(*port);

  if (bind(listener, (struct sockaddr *)&addr, sizeof addr) != 0) {
    LOG_ERRNO("bind");
    close(listener);
    return -1;
  }

  if (*port == 0) {
    struct sockaddr_in bound;
    socklen_t len = sizeof bound;
    if (getsockname(listener, (struct sockaddr *)&bound, &len) != 0) {
      LOG_ERRNO("getsockname");
      close(listener);
      return -1;
    }
    *port = ntohs(bound.sin_port);
  }

  if (listen(listener, KVS_LISTEN_BACKLOG) != 0) {
    LOG_ERRNO("listen");
    close(listener);
    return -1;
  }
  return listener;
}

typedef enum kvs_accept_result_t {
  KVS_ACCEPT_SUCCESS,
  KVS_ACCEPT_INTERRUPT,
  KVS_ACCEPT_DEFECT,
} kvs_accept_result_t;

static kvs_accept_result_t make_accept(int listener, int *socket, uint16_t *port) {
  struct sockaddr_in peer;
  socklen_t len = sizeof peer; // out arg

  while (true) {
    if (interrupted) {
      LOG_LISTENER("interrupted");
      return KVS_ACCEPT_INTERRUPT;
    }

    errno = 0;
    int served = accept(listener, (struct sockaddr *)&peer, &len);

    if (served < 0) {
      int error = errno;
      switch (error) {
      case EINTR:
        LOG_LISTENER("interrupted");
        return KVS_ACCEPT_INTERRUPT;
      case ECONNABORTED:
        TRACE_LISTENER("connection aborted: retry");
        continue;
      case EMFILE:
      case ENFILE:
      case ENOMEM:
      case ENOBUFS:
        TRACE_LISTENER("resource error: %s (%d). waiting %d seconds before retry...",
                       strerror(error), error, KVS_RETRY_DELAY_SECS);
        sleep(KVS_RETRY_DELAY_SECS);
        continue;
      default:
        LOG_LISTENER("defect error: %s (%d)", strerror(error), error);
        return KVS_ACCEPT_DEFECT;
      }
    }

    *socket = served;
    *port = ntohs(peer.sin_port);
    return KVS_ACCEPT_SUCCESS;
  }
}

static bool has_disallowed_chars(const char *buf, size_t n) {
  for (size_t i = 0; i < n; i++) {
    for (size_t j = 0; j < sizeof(KVS_DISALLOWED_CHARS) - 1; j++) {
      if (buf[i] == KVS_DISALLOWED_CHARS[j]) {
        return true;
      }
    }
  }
  return false;
}

static void handle_socket(kvs_server_t *server, int socket, uint16_t port) {
  char request_buf[KVS_MAX_LINE_LENGTH + 1];  // plus NUL
  char response_buf[KVS_MAX_LINE_LENGTH + 1]; // plus NUL

  (void)port; // used only in LOG_SOCKET
  LOG_SOCKET("opened (port: %d)", port);

  kvs_stream_t stream;
  kvs_stream_init(&stream, socket);

  while (true) {
    if (interrupted) {
      break;
    }

    size_t request_len;
    kvs_read_result_t read_result =
        kvs_read_line(&stream, request_buf, KVS_MAX_LINE_LENGTH, &request_len);

    kvs_error_t error = KVS_ERROR_COUNT;
    if (read_result == KVS_READ_LINE_TOO_LONG) {
      error = KVS_ERROR_LINE_TOO_LONG;
    } else if (read_result != KVS_READ_SUCCESS) {
      break;
    } else if (has_disallowed_chars(request_buf, request_len)) {
      error = KVS_ERROR_DISALLOWED_CHARS;
    }
    if (error != KVS_ERROR_COUNT) {
      const char *error_text = kvs_error_texts[error];
      if (kvs_write_line(&stream, error_text, strlen(error_text)) != KVS_WRITE_SUCCESS) {
        break;
      }
      continue;
    }

    request_buf[request_len] = '\0';
    LOG_SOCKET("request [%s]", request_buf);

    if (!server->handler(server, request_buf, response_buf, server->handler_ctx)) {
      LOG_SOCKET("closed by handler");
      break;
    }

    size_t response_len = strlen(response_buf);
    kvs_write_result_t write_result = kvs_write_line(&stream, response_buf, response_len);
    if (write_result != KVS_WRITE_SUCCESS) {
      break;
    }

    LOG_SOCKET("response [%s]", response_buf);
  }

  close(socket);
  LOG_SOCKET("closed");
}

void kvs_server_init(kvs_server_t *server, const char *address, uint16_t port,
                     kvs_server_request_handler_t *handler, void *handler_ctx) {
  assert(server != NULL);
  assert(address != NULL);
  assert(handler != NULL);

  server->address = strdup(address);
  server->port = port;
  server->listener = -1;
  server->handler = handler;
  server->handler_ctx = handler_ctx;
}

void kvs_server_free(kvs_server_t *server) {
  assert(server != NULL);

  free((void *)server->address);
}

bool kvs_server_start(kvs_server_t *server) {
  assert(server != NULL);

  int listener = server->listener;
  if (listener != -1) {
    LOG_LISTENER("server already started");
    return false;
  }

  listener = make_listener(server->address, &server->port);
  if (listener == -1) {
    return false;
  }
  LOG_LISTENER("started (port: %d)", server->port);

  setup_signals();

  server->listener = listener;
  return true;
}

void kvs_server_stop(kvs_server_t *server) {
  assert(server != NULL);

  int listener = server->listener;
  if (listener == -1) {
    return;
  }

  close(listener);
  LOG_LISTENER("stopped");
  server->listener = -1;
}

bool kvs_server_run(kvs_server_t *server) {
  assert(server != NULL);

  int listener = server->listener;
  if (listener == -1) {
    LOG_LISTENER("server not started");
    return false;
  }

  while (true) {
    if (interrupted) {
      break;
    }

    int socket;
    uint16_t port;
    kvs_accept_result_t accept_result = make_accept(listener, &socket, &port);
    if (accept_result == KVS_ACCEPT_INTERRUPT) {
      return true;
    }
    if (accept_result != KVS_ACCEPT_SUCCESS) {
      return false;
    }

    handle_socket(server, socket, port);
  }

  return true;
}
