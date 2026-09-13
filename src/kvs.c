#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "debug.h"
#include "protocol.h"
#include "server.h"
#include "table.h"

#define LOG(...) KVS_PRINT("server", __VA_ARGS__)

static bool handler(const kvs_server_t *server, char *request, char *response, void *ctx) {
  (void)server;
  kvs_table_t *table = ctx;

  assert(strlen(request) <= KVS_MAX_LINE_LENGTH);
  kvs_protocol_handle(table, request, response);
  assert(strlen(response) <= KVS_MAX_LINE_LENGTH);

  return true;
}

int main(void) {
  int ret = 0;

  kvs_table_t table;
  kvs_table_init(&table);

  kvs_server_t server;
  kvs_server_init(&server, get_address(), get_port(), handler, &table);

  if (!kvs_server_start(&server)) {
    LOG("start failed");
    kvs_server_free(&server);
    return 1;
  }
  LOG("listening %s %d", server.address, server.port);

  if (kvs_server_run(&server)) {
    LOG("run stopped");
    ret = 0;
  } else {
    LOG("run failed");
    ret = 1;
  }

  kvs_server_stop(&server);
  kvs_server_free(&server);
  kvs_table_free(&table);
  return ret;
}
