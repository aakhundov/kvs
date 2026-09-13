#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "table.h"

static inline void write_to_response(char *response, const char *prefix, const char *content) {
  if (content != NULL) {
    (void)sprintf(response, "%s %s", prefix, content);
  } else {
    (void)sprintf(response, "%s", prefix);
  }
}

static inline void write_error(char *response, kvs_error_t error) {
  write_to_response(response, kvs_error_texts[error], NULL);
}

static inline void write_value(char *response, const char *value) {
  write_to_response(response, "VAL", value);
}

static inline void write_text(char *response, const char *text) {
  write_to_response(response, text, NULL);
}

static void handle_get(kvs_table_t *table, const char *request, char *response) {
  if (*request == '\0') {
    write_error(response, KVS_ERROR_MISSING_KEY);
    return;
  }
  if (strchr(request, ' ') != NULL) {
    write_error(response, KVS_ERROR_SPACE_IN_KEY);
    return;
  }

  const char *key = request;
  const char *value;
  if (kvs_table_get(table, key, &value)) {
    if (value == NULL) {
      write_error(response, KVS_ERROR_ALLOCATION);
    } else {
      write_value(response, value);
      free((void *)value); // free owned
    }
  } else {
    write_text(response, "NIL");
  }
}

static void handle_set(kvs_table_t *table, const char *request, char *response) {
  char *sep = strchr(request, ' ');
  if (*request == '\0' || request == sep) {
    write_error(response, KVS_ERROR_MISSING_KEY);
    return;
  }
  if (sep == NULL) {
    write_error(response, KVS_ERROR_NO_SPACE_AFTER_KEY);
    return;
  }

  *sep = '\0'; // temp modify: end of key
  const char *key = request;
  const char *value = sep + 1;
  if (kvs_table_set(table, key, value)) {
    write_text(response, "OK");
  } else {
    write_error(response, KVS_ERROR_ALLOCATION);
  }
  *sep = ' '; // restore
}

static void handle_del(kvs_table_t *table, const char *request, char *response) {
  if (*request == '\0') {
    write_error(response, KVS_ERROR_MISSING_KEY);
    return;
  }
  if (strchr(request, ' ') != NULL) {
    write_error(response, KVS_ERROR_SPACE_IN_KEY);
    return;
  }

  const char *key = request;
  if (kvs_table_delete(table, key)) {
    write_text(response, "OK");
  } else {
    write_text(response, "NIL");
  }
}

void kvs_protocol_handle(kvs_table_t *table, char *request, char *response) {
  if (strlen(request) >= 4) {
    char *req_content = request + 4;
    if (memcmp(request, "GET ", 4) == 0) {
      handle_get(table, req_content, response);
      return;
    }
    if (memcmp(request, "SET ", 4) == 0) {
      handle_set(table, req_content, response);
      return;
    }
    if (memcmp(request, "DEL ", 4) == 0) {
      handle_del(table, req_content, response);
      return;
    }
  }

  write_error(response, KVS_ERROR_BAD_REQUEST);
}
