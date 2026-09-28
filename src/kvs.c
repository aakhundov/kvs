#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "logger.h"
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
  bool failed_to_init_logger = false;
  bool failed_to_stop = false;

  // connection threads write log via rings
  if (!kvs_logger_init(get_max_connections())) {
    (void)fprintf(stderr, "failed to init logger\n");
    failed_to_init_logger = true;
    ret = 1;
    goto out;
  }

  static kvs_table_t table;
  kvs_table_init(&table);

  static kvs_server_t server;
  kvs_server_init(&server, get_address(), get_port(),
                  (kvs_server_config_t){
                      .max_connections = get_max_connections(),
                      .stop_timeout = get_stop_timeout(),
                      .handler = handler,
                      .handler_ctx = &table,
                  });

  if (!kvs_server_start(&server)) {
    LOG("start failed");
    ret = 1;
    goto out;
  }

  LOG("listening %s %d", kvs_server_get_address(&server), kvs_server_get_port(&server));

  if (kvs_server_run(&server)) {
    LOG("run interrupted");
  } else {
    LOG("run failed");
    ret = 1;
  }

  if (kvs_server_stop(&server)) {
    LOG("server stopped");
  } else {
    LOG("stopping failed");
    failed_to_stop = true;
    ret = 1;
  }

out:
  if (!failed_to_init_logger) {
    if (!failed_to_stop) {
      // free server and table only if all connection
      // handler threads were stopped successfully
      kvs_server_free(&server);
      kvs_table_free(&table);
    }

    // free rings only when connection
    // threads were stopped successfully
    kvs_logger_free(!failed_to_stop);
  }

  return ret;
}
