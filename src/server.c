// for pthread_setname_np
#define _GNU_SOURCE // NOLINT(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
                    // pthread_setname_np

#include "server.h"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
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

#define MAX_THREAD_NAME_LENGTH 16

static volatile sig_atomic_t interrupted = 0;

static void on_interrupt(int sig) {
  (void)sig;
  interrupted = 1;
}

static inline void setup_signals(void) {
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

static inline void mask_signals(bool mask) {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  pthread_sigmask(mask ? SIG_BLOCK : SIG_UNBLOCK, &set, NULL);
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
    struct sockaddr_in bound = {0};
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
  struct sockaddr_in peer = {0};
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

static inline bool has_disallowed_chars(const char *buf, size_t n) {
  for (size_t i = 0; i < n; i++) {
    for (size_t j = 0; j < sizeof(KVS_DISALLOWED_CHARS) - 1; j++) {
      if (buf[i] == KVS_DISALLOWED_CHARS[j]) {
        return true;
      }
    }
  }
  return false;
}

static inline kvs_write_result_t write_error(int socket, kvs_error_t error) {
  kvs_stream_t stream;
  kvs_stream_init(&stream, socket);
  const char *error_text = kvs_error_texts[error];
  return kvs_write_line(&stream, error_text, strlen(error_text));
}

static inline void register_socket_locked(kvs_server_t *server, int socket) {
  assert(socket != -1);

  for (size_t i = 0; i < server->config.max_connections; i++) {
    if (server->sockets[i] == -1) {
      server->sockets[i] = socket;
      return;
    }
  }

  // there must have been at least one zero item
  assert(0 && "unreachable");
}

static inline void unregister_socket_locked(kvs_server_t *server, int socket) {
  assert(socket != -1);

  for (size_t i = 0; i < server->config.max_connections; i++) {
    if (server->sockets[i] == socket) {
      server->sockets[i] = -1;
      return;
    }
  }

  // socket must have been added before being removed
  assert(0 && "unreachable");
}

static inline struct timespec after(uint16_t seconds) {
  struct timespec result;
  clock_gettime(CLOCK_REALTIME, &result);
  result.tv_sec += seconds;
  return result;
}

static void *handle_connection(void *arg) {
  kvs_connection_t *connection = arg; // owned
  int socket = connection->socket;
  uint16_t port = connection->port;
  kvs_server_t *server = connection->server;
  kvs_server_request_handler_t *request_handler = server->config.handler;
  void *handler_ctx = server->config.handler_ctx;

  char request_buf[KVS_MAX_LINE_LENGTH + 1];  // plus NUL
  char response_buf[KVS_MAX_LINE_LENGTH + 1]; // plus NUL

  char name[MAX_THREAD_NAME_LENGTH + 1];
  (void)snprintf(name, MAX_THREAD_NAME_LENGTH, "conn %d (%d)", socket, port);
  pthread_setname_np(pthread_self(), name); // from _GNU_SOURCE

  LOG_SOCKET("started (port: %d)", port);

  kvs_stream_t stream;
  kvs_stream_init(&stream, socket);

  bool failed = false;
  size_t num_requests = 0;

  while (true) {
    size_t request_len;
    kvs_read_result_t read_result =
        kvs_read_line(&stream, request_buf, KVS_MAX_LINE_LENGTH, &request_len);

    kvs_error_t client_error = KVS_ERROR_COUNT; // sentinel
    if (read_result == KVS_READ_LINE_TOO_LONG) {
      client_error = KVS_ERROR_LINE_TOO_LONG;
    } else if (read_result != KVS_READ_SUCCESS) {
      if (read_result != KVS_READ_INTERRUPT && read_result != KVS_READ_END_OF_INPUT) {
        failed = true;
      }
      break;
    } else if (has_disallowed_chars(request_buf, request_len)) {
      client_error = KVS_ERROR_DISALLOWED_CHARS;
    }
    if (client_error != KVS_ERROR_COUNT) {
      kvs_write_result_t write_result = write_error(socket, client_error);
      if (write_result != KVS_WRITE_SUCCESS) {
        if (write_result != KVS_WRITE_INTERRUPT) {
          failed = true;
        }
        break;
      }
      continue;
    }

    request_buf[request_len] = '\0';
    LOG_SOCKET("request [%s]", request_buf);

    if (!request_handler(server, request_buf, response_buf, handler_ctx)) {
      LOG_SOCKET("closed by handler");
      failed = true;
      break;
    }

    num_requests++;

    size_t response_len = strlen(response_buf);
    kvs_write_result_t write_result = kvs_write_line(&stream, response_buf, response_len);
    if (write_result != KVS_WRITE_SUCCESS) {
      if (write_result != KVS_WRITE_INTERRUPT) {
        failed = true;
      }
      break;
    }

    LOG_SOCKET("response [%s]", response_buf);
  }

  close(socket);
  free(connection); // free owned

  (void)failed;       // used only in logging
  (void)num_requests; // used only in logging
  LOG_SOCKET("%zu requests processed", num_requests);
  LOG_SOCKET(failed ? "failed" : "stopped");

  pthread_mutex_lock(&server->lock);
  server->num_connections--;
  unregister_socket_locked(server, socket);
  pthread_cond_signal(&server->stop_var);
  pthread_mutex_unlock(&server->lock);

  return NULL;
}

static bool make_handler(kvs_server_t *server, int socket, uint16_t port) {
  kvs_connection_t *connection = malloc(sizeof(*connection));
  if (connection == NULL) {
    return false; // allocaiton failed
  }

  connection->server = server;
  connection->socket = socket;
  connection->port = port;

  // temporarily mask signals so that the connection
  // thread inherits blocked interrupt signal handling
  mask_signals(true);

  bool ret = true;
  pthread_t thread;
  // the connection is owned (and freed) by the thread
  if (pthread_create(&thread, NULL, handle_connection, connection) != 0) {
    free(connection); // free, as no thread to free it
    ret = false;      // thread creation failed
    goto out;
  }
  // the thread is on its own
  pthread_detach(thread);

out:
  mask_signals(false); // unmask signals
  return ret;
}

static bool stop_handlers(kvs_server_t *server) {
  bool ret = true;
  pthread_mutex_lock(&server->lock);

  if (server->num_connections > 0) {
    for (size_t i = 0; i < server->config.max_connections; i++) {
      if (server->sockets[i] != -1) {
        shutdown(server->sockets[i], SHUT_RDWR);
      }
    }

    struct timespec deadline = after(server->config.stop_timeout);

    while (server->num_connections > 0) {
      if (pthread_cond_timedwait(&server->stop_var, &server->lock, &deadline) != 0) {
        ret = false; // timeout
        break;
      }
    }
  }

  pthread_mutex_unlock(&server->lock);
  return ret;
}

void kvs_server_init(kvs_server_t *server, const char *address, uint16_t port,
                     kvs_server_config_t config) {
  assert(server != NULL);
  assert(address != NULL);
  assert(config.handler != NULL);

  server->address = strdup(address);
  server->port = port;
  server->listener = -1;
  server->config = config;
  server->num_connections = 0;

  server->sockets = malloc(server->config.max_connections * sizeof(*server->sockets));
  for (size_t i = 0; i < server->config.max_connections; i++) {
    server->sockets[i] = -1;
  }

  pthread_mutex_init(&server->lock, NULL);
  pthread_cond_init(&server->stop_var, NULL);
}

void kvs_server_free(kvs_server_t *server) {
  assert(server != NULL);

  free((void *)server->address);
  free(server->sockets);

  pthread_mutex_destroy(&server->lock);
  pthread_cond_destroy(&server->stop_var);
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

bool kvs_server_stop(kvs_server_t *server) {
  assert(server != NULL);

  int listener = server->listener;
  if (listener == -1) {
    return true;
  }

  close(listener);
  LOG_LISTENER("stopped");
  server->listener = -1;

  bool ret = true;
  if (!stop_handlers(server)) {
    LOG_LISTENER("failed to stop handlers (timeout: %d sec)", server->config.stop_timeout);
    ret = false;
  } else {
    LOG_LISTENER("handlers stopped");
  }

  return ret;
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
      break;
    }
    if (accept_result != KVS_ACCEPT_SUCCESS) {
      return false;
    }

    bool available = true;
    pthread_mutex_lock(&server->lock);
    if (server->num_connections < server->config.max_connections) {
      server->num_connections++;
      register_socket_locked(server, socket);
    } else {
      available = false;
    }
    pthread_mutex_unlock(&server->lock);

    if (!available) {
      LOG_LISTENER("no connection for %d (limit: %d)", socket, server->config.max_connections);
      write_error(socket, KVS_ERROR_CONNECTION_LIMIT);
      close(socket);
    } else if (!make_handler(server, socket, port)) {
      pthread_mutex_lock(&server->lock);
      server->num_connections--;
      unregister_socket_locked(server, socket);
      pthread_mutex_unlock(&server->lock);

      LOG_LISTENER("failed to make handler for %d", socket);
      write_error(socket, KVS_ERROR_FAILED_TO_HANDLE);
      close(socket);
    }
  }

  return true;
}
