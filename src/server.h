#ifndef KVS_SERVER_H
#define KVS_SERVER_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pthread.h>

#define KVS_LISTEN_BACKLOG 16
#define KVS_RETRY_DELAY_SECS 10
#define KVS_MAX_LINE_LENGTH 1024
#define KVS_DISALLOWED_CHARS "\r\0"

typedef struct kvs_server_t kvs_server_t;

// (request) is a NUL-terminated string containing the request line,
// free of the trailing \r or \n chars. (response) is to be filled with
// response line: a NUL-terminated string up to KVS_MAX_LINE_LENGTH chars,
// not including the NUL char. return value means request successfully
// processed: when false is returned, underlying connection is closed.
// modifying (request) buffer within the C-string bounds is allowed.
typedef bool kvs_server_request_handler_t(const kvs_server_t *server, char *request, char *response,
                                          void *ctx);

typedef struct kvs_server_config_t {
  uint16_t max_connections;
  uint16_t stop_timeout;
  kvs_server_request_handler_t *handler;
  void *handler_ctx;
} kvs_server_config_t;

typedef struct kvs_connection_t {
  bool assigned;
  atomic_bool active;
  pthread_t thread;
  kvs_server_t *server;
  int socket;
  uint16_t port;
  size_t num_requests;
  bool failed;
  unsigned long stream;
} kvs_connection_t;

typedef struct kvs_statistics_t {
  size_t connections_accepted;
  size_t connections_started;
  size_t connections_finished;
  size_t connections_failed;
  size_t requests_processed;
} kvs_statistics_t;

typedef struct kvs_server_t {
  const char *address;
  uint16_t port;
  kvs_server_config_t config;
  int listener;
  kvs_statistics_t counters;
  kvs_connection_t *connections;
  // access to the members below
  // is protected by the (lock)
  size_t num_active_connections;
  pthread_cond_t stop_var;
  pthread_mutex_t lock;
} kvs_server_t;

void kvs_server_init(kvs_server_t *server, const char *address, uint16_t port,
                     kvs_server_config_t config);
void kvs_server_free(kvs_server_t *server);
bool kvs_server_start(kvs_server_t *server);
bool kvs_server_run(kvs_server_t *server);
bool kvs_server_stop(kvs_server_t *server);
const char *kvs_server_get_address(const kvs_server_t *server);
uint16_t kvs_server_get_port(const kvs_server_t *server);

#endif
