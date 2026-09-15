#ifndef KVS_SERVER_H
#define KVS_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
// user should make sure (server) still exists before dereferencing it.
typedef bool kvs_server_request_handler_t(const kvs_server_t *server, char *request, char *response,
                                          void *ctx);

typedef struct kvs_server_t {
  const char *address;
  uint16_t port;
  int listener;
  kvs_server_request_handler_t *handler;
  void *handler_ctx;
} kvs_server_t;

typedef struct kvs_connection_t {
  // (server) is opaque pointer to pass to the handler:
  // not to be used as a server instance otherwise
  void *server;
  int socket;
  uint16_t port;
  kvs_server_request_handler_t *handler;
  void *handler_ctx;
} kvs_connection_t;

void kvs_server_init(kvs_server_t *server, const char *address, uint16_t port,
                     kvs_server_request_handler_t *handler, void *handler_ctx);
void kvs_server_free(kvs_server_t *server);
bool kvs_server_start(kvs_server_t *server);
bool kvs_server_run(kvs_server_t *server);
void kvs_server_stop(kvs_server_t *server);

#endif
