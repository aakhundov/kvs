#ifndef KVS_TABLE_H
#define KVS_TABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pthread.h>

typedef uint32_t kvs_hash_t;

typedef struct kvs_table_entry_t {
  const char *key;
  const char *value;
  kvs_hash_t hash;
} kvs_table_entry_t;

typedef struct kvs_table_t {
  size_t length;
  size_t capacity;
  kvs_table_entry_t *entries;
  // the (lock) covers access to the
  // members above and through them
  pthread_mutex_t lock;
} kvs_table_t;

// init / free are table constructor / destructor.
// calls to all other public API functions are valid
// only after a call to kvs_table_init and before a
// subsequent call to kvs_table_free. init / free are
// not idempotent: calling any of them again, before
// calling the counterpart, is undefined.
void kvs_table_init(kvs_table_t *table);
void kvs_table_free(kvs_table_t *table);

// gets the (*value) by the (key) from the (table). if present,
// writes into (*value) and returns true. if absent, returns
// (false). (key) and (*value) are NUL-terminated strings.
// (key) is client-owned. the C-string (*value) is moved to
// the client and must be freed by the client. (*value) is
// set to NULL when returning true if allocating a copy of
// the value string has failed.
bool kvs_table_get(kvs_table_t *table, const char *key, const char **value);

// sets the (value) by the (key) in the (table). returns true
// on success (both for new entry and overwrite), false on failed
// memory allocation. (key) and (value) are both client-owned NUL-
// terminated strings: both are copied into the table.
bool kvs_table_set(kvs_table_t *table, const char *key, const char *value);

// deletes the entry by the (key) from the (table). returns true
// if the (key) was present (and deleted), false if absent (and not
// deleted). (key) is client-owned NUL-terminated string.
bool kvs_table_delete(kvs_table_t *table, const char *key);

#endif
