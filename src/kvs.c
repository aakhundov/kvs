#include <assert.h>
#include <stddef.h>
#include <string.h>

#include <pthread.h>

#include "config.h"
#include "debug.h"
#include "protocol.h"
#include "server.h"
#include "table.h"

#define LOG(...) KVS_PRINT("server", __VA_ARGS__)

static pthread_mutex_t table_lock = PTHREAD_MUTEX_INITIALIZER;

static bool handler(const kvs_server_t *server, char *request, char *response, void *ctx) {
  (void)server;
  kvs_table_t *table = ctx;

  assert(strlen(request) <= KVS_MAX_LINE_LENGTH);

  pthread_mutex_lock(&table_lock);
  kvs_protocol_handle(table, request, response);
  pthread_mutex_unlock(&table_lock);

  assert(strlen(response) <= KVS_MAX_LINE_LENGTH);

  return true;
}

int main(void) {
  int ret = 0;

  kvs_table_t table;
  pthread_mutex_lock(&table_lock);
  kvs_table_init(&table);
  pthread_mutex_unlock(&table_lock);

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

  pthread_mutex_lock(&table_lock);
  kvs_table_free(&table);
  pthread_mutex_unlock(&table_lock);

  return ret;
}
