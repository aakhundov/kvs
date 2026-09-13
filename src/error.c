#include "error.h"

#include "server.h"

const char *kvs_error_texts[] = {
#define X(name, text) [KVS_ERROR_##name] = (KVS_ERROR_PREFIX text),
#include "errors.def"
#undef X
};

#define X(name, text)                                                                              \
  _Static_assert(sizeof(KVS_ERROR_PREFIX) - 1 + sizeof(text) - 1 <= KVS_MAX_LINE_LENGTH,           \
                 "full error text should fit in server's maximum line length");
#include "errors.def"
#undef X
