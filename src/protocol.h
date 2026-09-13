#ifndef KVS_PROTOCOL_H
#define KVS_PROTOCOL_H

#include "table.h"

// implements KVS protocol API. (request) and (response)
// mirror KVS server handler's API rules and constraints.
// the implementation relies on two invariants: (1) max.
// request and response lengths are the same; (2) every
// error fits into the max. response length, including
// the "ERR " prefix.
void kvs_protocol_handle(kvs_table_t *table, char *request, char *response);

#endif
