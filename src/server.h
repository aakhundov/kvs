#ifndef KVS_SERVER_H
#define KVS_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KVS_LISTEN_BACKLOG 16
#define KVS_RETRY_DELAY_SECS 10
#define KVS_MAX_LINE_LENGTH 1024

typedef struct kvs_server_t kvs_server_t;

// (request) is a string buffer (not NUL-terminated) containing the
// request line of length (request_len), free of the trailing \r or
// \n chars. (response) is to be filled with response line (not NUL-
// terminated), up to (max_response_len) chars. the actual length of
// the response line must be written into (response_len). return value
// means request successfully processed: when false is returned, the
// underlying socket (and connection) are closed.
typedef bool kvs_server_request_handler_t(const kvs_server_t *server, const char *request,
                                          size_t request_len, char *response,
                                          size_t max_response_len, size_t *response_len, void *ctx);

typedef struct kvs_server_t {
  const char *address;
  uint16_t port;
  int listener;
  kvs_server_request_handler_t *handler;
  void *handler_ctx;
} kvs_server_t;

void kvs_server_init(kvs_server_t *server, const char *address, uint16_t port,
                     kvs_server_request_handler_t *handler, void *handler_ctx);
void kvs_server_free(kvs_server_t *server);
bool kvs_server_start(kvs_server_t *server);
bool kvs_server_run(kvs_server_t *server);
void kvs_server_stop(kvs_server_t *server);

#endif
