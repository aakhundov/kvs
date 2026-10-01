// for pthread_setname_np
#define _GNU_SOURCE // NOLINT(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
                    // pthread_setname_np

#include "server.h"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
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
#include "logger.h"

#define LOG_ERROR(function, ...) KVS_LOG(function, __VA_ARGS__)
#define LOG_ERRNO(function) KVS_LOG(function, "%s (%d)", strerror(errno), errno)

#define LOG_LISTENER(...) KVS_LOG_WITH_ID("listener", listener, __VA_ARGS__)
#define LOG_SOCKET(...) KVS_LOG_WITH_ID("socket", socket, __VA_ARGS__)
#define TRACE_LISTENER(...) KVS_TRACE_WITH_ID("listener", listener, __VA_ARGS__)
#define TRACE_SOCKET(...) KVS_TRACE_WITH_ID("socket", socket, __VA_ARGS__)

#define MAX_THREAD_NAME_LENGTH 16
#define MAIN_THREAD_STREAM 1000

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

static kvs_accept_result_t accept_connection(int listener, int *socket, uint16_t *port) {
  struct sockaddr_in peer = {0};
  socklen_t len = sizeof peer; // out arg

  while (true) {
    SCHED_POINT();
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
  SCHED_POINT();
  return kvs_write_line(&stream, error_text, strlen(error_text));
}

static inline struct timespec after(uint16_t seconds) {
  struct timespec result;
  clock_gettime(CLOCK_REALTIME, &result);
  result.tv_sec += seconds;
  return result;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void *connection_handler(void *arg) {
  kvs_connection_t *conn = arg;
  int socket = conn->socket;
  uint16_t port = conn->port;
  kvs_server_t *server = conn->server;
  kvs_server_request_handler_t *request_handler = server->config.handler;
  void *handler_ctx = server->config.handler_ctx;

  char request_buf[KVS_MAX_LINE_LENGTH + 1];  // plus NUL
  char response_buf[KVS_MAX_LINE_LENGTH + 1]; // plus NUL

  char name[MAX_THREAD_NAME_LENGTH + 1];
  (void)snprintf(name, MAX_THREAD_NAME_LENGTH, "conn %d (%d)", socket, port);
  pthread_setname_np(pthread_self(), name); // from _GNU_SOURCE

  SET_THREAD_STREAM(conn->stream);

  SCHED_POINT();
  // start async logging from connection thread
  bool async_logging = kvs_logger_async_attach();
  assert(async_logging);
  (void)async_logging;

  LOG_SOCKET("connection started (port: %d)", port);

  kvs_stream_t stream;
  kvs_stream_init(&stream, socket);

  bool failed = false;
  size_t num_requests = 0;

  while (true) {
    size_t request_len;
    SCHED_POINT();
    kvs_read_result_t read_result =
        kvs_read_line(&stream, request_buf, KVS_MAX_LINE_LENGTH, &request_len);
    SCHED_POINT();

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
    SCHED_POINT();
    kvs_write_result_t write_result = kvs_write_line(&stream, response_buf, response_len);
    if (write_result != KVS_WRITE_SUCCESS) {
      if (write_result != KVS_WRITE_INTERRUPT) {
        failed = true;
      }
      break;
    }

    LOG_SOCKET("response [%s]", response_buf);
  }

  SCHED_POINT();
  close(socket);

  conn->failed = failed;
  conn->num_requests = num_requests;

  LOG_SOCKET("%ld requests processed", num_requests);
  LOG_SOCKET("connection %s", failed ? "failed" : "finished");

  SCHED_POINT();
  kvs_logger_async_detach();
  SCHED_POINT();

  atomic_store_explicit(&conn->active, false, memory_order_relaxed);

  SCHED_POINT();
  pthread_mutex_lock(&server->lock);
  server->num_active_connections--;
  pthread_cond_signal(&server->stop_var);
  pthread_mutex_unlock(&server->lock);
  SCHED_POINT();

  return NULL;
}

static inline kvs_connection_t *get_available_connection(kvs_server_t *server) {
  for (kvs_connection_t *conn = server->connections;
       conn < server->connections + server->config.max_connections; conn++) {
    if (!atomic_load_explicit(&conn->active, memory_order_relaxed)) {
      return conn;
    }
  }

  return NULL;
}

static inline void finalize_handler(kvs_server_t *server, kvs_connection_t *conn) {
  SCHED_POINT();
  pthread_join(conn->thread, NULL);

  server->counters.connections_finished++;
  server->counters.requests_processed += conn->num_requests;
  if (conn->failed) {
    server->counters.connections_failed++;
  }
}

static bool make_handler(kvs_server_t *server, kvs_connection_t *conn, int socket, uint16_t port) {
  if (conn->assigned) {
    finalize_handler(server, conn);
    conn->assigned = false;
  }

  conn->server = server;
  conn->socket = socket;
  conn->port = port;

  // connection thread's stream is set based on the # of accepted connections
  conn->stream = MAIN_THREAD_STREAM + server->counters.connections_accepted;

  // temporarily mask signals so that the connection
  // thread inherits blocked interrupt signal handling
  mask_signals(true);

  pthread_t thread;
  bool thread_created = true;
  atomic_store_explicit(&conn->active, true, memory_order_relaxed);
  if (pthread_create(&thread, NULL, connection_handler, conn) != 0) {
    atomic_store_explicit(&conn->active, false, memory_order_relaxed);
    thread_created = false;
    goto out;
  }
  SCHED_POINT();

  conn->thread = thread;
  conn->assigned = true;

  server->counters.connections_started++;

out:
  mask_signals(false); // unmask signals
  return thread_created;
}

static bool stop_handlers(kvs_server_t *server) {
  bool threads_stopped = true;
  SCHED_POINT();
  pthread_mutex_lock(&server->lock);

  if (server->num_active_connections > 0) {
    for (kvs_connection_t *conn = server->connections;
         conn < server->connections + server->config.max_connections; conn++) {
      if (atomic_load_explicit(&conn->active, memory_order_relaxed)) {
        shutdown(conn->socket, SHUT_RDWR);
      }
    }

    struct timespec deadline = after(server->config.stop_timeout);

    while (server->num_active_connections > 0) {
      if (pthread_cond_timedwait(&server->stop_var, &server->lock, &deadline) != 0) {
        threads_stopped = false; // timeout
        break;
      }
    }
  }

  pthread_mutex_unlock(&server->lock);

  if (threads_stopped) {
    for (kvs_connection_t *conn = server->connections;
         conn < server->connections + server->config.max_connections; conn++) {
      if (conn->assigned) {
        finalize_handler(server, conn);
        conn->assigned = false;
      }
    }
  }

  return threads_stopped;
}

static inline void log_statistics(kvs_server_t *server, bool partial) {
  int listener = server->listener;
  (void)listener;

  LOG_LISTENER("server statistics (%s):", partial ? "partial" : "complete");

  if (partial) {
    SCHED_POINT();
    pthread_mutex_lock(&server->lock);
    size_t connections_pending = server->num_active_connections;
    pthread_mutex_unlock(&server->lock);
    SCHED_POINT();

    LOG_LISTENER("* connections pending: %zu", connections_pending);
    (void)connections_pending;
  }

  LOG_LISTENER("* connections accepted: %zu", server->counters.connections_accepted);
  LOG_LISTENER("* connections started: %zu", server->counters.connections_started);
  LOG_LISTENER("* connections finished: %zu", server->counters.connections_finished);
  LOG_LISTENER("* connections failed: %zu", server->counters.connections_failed);
  LOG_LISTENER("* requests processed: %zu", server->counters.requests_processed);
}

void kvs_server_init(kvs_server_t *server, const char *address, uint16_t port,
                     kvs_server_config_t config) {
  assert(server != NULL);
  assert(address != NULL);
  assert(config.handler != NULL);

  SET_THREAD_STREAM(MAIN_THREAD_STREAM);

  server->address = strdup(address);
  server->port = port;
  server->listener = -1;
  server->config = config;
  server->num_active_connections = 0;
  server->counters = (kvs_statistics_t){0};

  server->connections = malloc(server->config.max_connections * sizeof(*server->connections));
  for (kvs_connection_t *conn = server->connections;
       conn < server->connections + server->config.max_connections; conn++) {
    atomic_init(&conn->active, false);
    conn->assigned = false;
  }

  pthread_mutex_init(&server->lock, NULL);
  pthread_cond_init(&server->stop_var, NULL);
}

void kvs_server_free(kvs_server_t *server) {
  assert(server != NULL);

  free((void *)server->address);
  free(server->connections);

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
  LOG_LISTENER("listener started (port: %d)", server->port);

  setup_signals();

  server->listener = listener;
  return true;
}

bool kvs_server_stop(kvs_server_t *server) {
  assert(server != NULL);

  int listener = server->listener;
  if (listener == -1) {
    LOG_LISTENER("server already stopped");
    return true;
  }

  close(listener);
  LOG_LISTENER("listener stopped");

  bool handlers_stopped = true;
  if (!stop_handlers(server)) {
    LOG_LISTENER("failed to stop handlers (timeout: %d sec)", server->config.stop_timeout);
    handlers_stopped = false;
  } else {
    LOG_LISTENER("handlers stopped");
  }

  log_statistics(server, !handlers_stopped);
  server->listener = -1;

  return handlers_stopped;
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
    kvs_accept_result_t accept_result = accept_connection(listener, &socket, &port);
    if (accept_result == KVS_ACCEPT_INTERRUPT) {
      break;
    }
    if (accept_result != KVS_ACCEPT_SUCCESS) {
      return false;
    }

    server->counters.connections_accepted++;

    bool available = false;
    SCHED_POINT();
    pthread_mutex_lock(&server->lock);
    if (server->num_active_connections < server->config.max_connections) {
      server->num_active_connections++;
      available = true;
    }
    pthread_mutex_unlock(&server->lock);
    SCHED_POINT();

    if (available) {
      kvs_connection_t *conn = get_available_connection(server);
      assert(conn != NULL); // connection must be available

      if (!make_handler(server, conn, socket, port)) {
        pthread_mutex_lock(&server->lock);
        server->num_active_connections--;
        pthread_mutex_unlock(&server->lock);

        LOG_LISTENER("failed to make handler for %d", socket);
        write_error(socket, KVS_ERROR_FAILED_TO_HANDLE);
        close(socket);
      }
    } else {
      LOG_LISTENER("no connection for %d (limit: %d)", socket, server->config.max_connections);
      write_error(socket, KVS_ERROR_CONNECTION_LIMIT);
      close(socket);
    }
  }

  return true;
}

const char *kvs_server_get_address(const kvs_server_t *server) {
  return server->address;
}

uint16_t kvs_server_get_port(const kvs_server_t *server) {
  return server->port;
}
