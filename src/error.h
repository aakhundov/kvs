#ifndef KVS_ERROR_H
#define KVS_ERROR_H

#define KVS_ERROR_PREFIX "ERR "

typedef enum kvs_error_t {
#define X(name, text) KVS_ERROR_##name,
#include "errors.def"
#undef X
  KVS_ERROR_COUNT,
} kvs_error_t;

extern const char *kvs_error_texts[];

#endif
