#include <stddef.h>
#include <stdio.h>

#include "config.h"
#include "debug.h"
#include "server.h"

#define LOG(...) KVS_PRINT("server", __VA_ARGS__)

static bool handler(const kvs_server_t *server, const char *request, size_t request_len,
                    char *response, size_t max_response_len, size_t *response_len, void *ctx) {
  (void)server;
  (void)max_response_len;
  (void)ctx;

  *response_len = (request_len <= max_response_len) ? request_len : max_response_len;

  // response is reversed request
  for (size_t i = 0; i < *response_len; i++) {
    response[i] = request[request_len - i - 1];
  }

  return true;
}

int main(void) {
  int ret = 0;

  kvs_server_t server;
  kvs_server_init(&server, get_address(), get_port(), handler, NULL);

  LOG("starting...");
  if (!kvs_server_start(&server)) {
    LOG("start failed");
    kvs_server_free(&server);
    return 1;
  }
  LOG("started on %s:%d", server.address, server.port);

  LOG("running...");
  if (kvs_server_run(&server)) {
    LOG("run exited");
    ret = 0;
  } else {
    LOG("run failed");
    ret = 1;
  }

  LOG("stopping...");
  kvs_server_stop(&server);
  LOG("stopped");

  kvs_server_free(&server);
  return ret;
}
