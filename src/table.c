#include "table.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_CAPACITY 8
#define NEXT_CAPACITY(prev_capacity)                                                               \
  ((prev_capacity) < INITIAL_CAPACITY ? INITIAL_CAPACITY : (prev_capacity) * 2)

#define MAX_LENGTH(capacity) ((capacity) / 2)
#define TOMBSTONE "tombstone"

#define FNV_1A_HASH_INIT 2166136261U
#define FNV_1A_HASH_FACTOR 16777619U

static inline kvs_hash_t hash_string(const char *chars) {
  char c;
  kvs_hash_t hash = FNV_1A_HASH_INIT;
  while ((c = *chars++)) {
    hash ^= (unsigned char)c;
    hash *= FNV_1A_HASH_FACTOR;
  }
  return hash;
}

static inline kvs_table_entry_t *find_entry(kvs_table_entry_t *entries, size_t capacity,
                                            const char *key, kvs_hash_t hash) {
  assert(capacity > 0);

  size_t index = hash & (capacity - 1);
  kvs_table_entry_t *entry = entries + index;
  kvs_table_entry_t *tombstone = NULL;

  while (1) {
    if (entry->key == NULL) {
      if (entry->value == NULL) { // non-tombstone
        // key not found
        // if tombstone found on the path, return it instead
        return (tombstone != NULL) ? tombstone : entry;
      }
      if (tombstone == NULL) {
        // first tombstone on the path
        tombstone = entry;
      }
    } else if (entry->hash == hash && strcmp(entry->key, key) == 0) {
      // key found
      return entry;
    }

    index++;
    entry++;
    if (index >= capacity) {
      // wrap around
      index = 0;
      entry = entries;
    }
  }
}

static inline bool adjust_capacity(kvs_table_t *t, size_t new_capacity) {
  // allocate new storage
  kvs_table_entry_t *new_entries = malloc(new_capacity * sizeof(*new_entries));
  if (new_entries == NULL) {
    return false; // allocation error
  }

  // clear new storage
  for (kvs_table_entry_t *e = new_entries; e < new_entries + new_capacity; e++) {
    e->key = NULL;
    e->value = NULL;
  }

  size_t new_length = 0;
  if (t->entries != NULL) {
    // copy the non-empty (and non-tombstone) entries from old to storage
    for (kvs_table_entry_t *src = t->entries; src < t->entries + t->capacity; src++) {
      if (src->key != NULL) {
        kvs_table_entry_t *dst = find_entry(new_entries, new_capacity, src->key, src->hash);
        *dst = *src; // copy the entry
        new_length++;
      }
    }
  }

  // free old storage
  free(t->entries);

  t->entries = new_entries;
  t->capacity = new_capacity;
  t->length = new_length;

  return true;
}

void kvs_table_init(kvs_table_t *table) {
  assert(table != NULL);

  table->length = 0;
  table->capacity = 0;
  table->entries = NULL;
}

void kvs_table_free(kvs_table_t *table) {
  assert(table != NULL);

  if (table->entries != NULL) {
    for (kvs_table_entry_t *e = table->entries; e < table->entries + table->capacity; e++) {
      if (e->key != NULL) {
        free((void *)e->key);
        free((void *)e->value);
      }
    }
    free((void *)table->entries);
  }

  table->length = 0;
  table->capacity = 0;
  table->entries = NULL;
}

bool kvs_table_get(const kvs_table_t *table, const char *key, const char **value) {
  assert(table != NULL);
  assert(key != NULL);
  assert(value != NULL);

  if (table->length == 0) {
    return false;
  }

  kvs_table_entry_t *entry = find_entry(table->entries, table->capacity, key, hash_string(key));
  if (entry->key == NULL) {
    return false;
  }

  // not owned: moved to client
  *value = strdup(entry->value);

  return true;
}

bool kvs_table_set(kvs_table_t *table, const char *key, const char *value) {
  assert(table != NULL);
  assert(key != NULL);
  assert(value != NULL);

  assert(table->length < SIZE_MAX);
  if (table->length + 1 > MAX_LENGTH(table->capacity)) {
    size_t new_capacity = NEXT_CAPACITY(table->capacity);
    if (!adjust_capacity(table, new_capacity)) {
      return false;
    }
  }

  kvs_hash_t hash = hash_string(key);
  kvs_table_entry_t *entry = find_entry(table->entries, table->capacity, key, hash);

  const char *new_value = strdup(value); // owned
  if (new_value == NULL) {
    return false;
  }
  const char *maybe_new_key = entry->key;
  if (maybe_new_key == NULL) {
    maybe_new_key = strdup(key); // owned
    if (maybe_new_key == NULL) {
      free((void *)new_value);
      return false;
    }
    if (entry->value == NULL) {
      // increment only when non-tombstone
      table->length++;
    }
  } else {
    free((void *)entry->value);
  }

  entry->key = maybe_new_key;
  entry->value = new_value;
  entry->hash = hash;

  return true;
}

bool kvs_table_delete(kvs_table_t *table, const char *key) {
  assert(table != NULL);
  assert(key != NULL);

  if (table->length == 0) {
    return false;
  }

  kvs_table_entry_t *entry = find_entry(table->entries, table->capacity, key, hash_string(key));
  if (entry->key == NULL) {
    return false;
  }

  free((void *)entry->key);
  free((void *)entry->value);

  // place a tombstone
  entry->key = NULL;
  entry->value = TOMBSTONE;

  return true;
}
