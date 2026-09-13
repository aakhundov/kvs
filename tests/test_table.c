#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <utest.h>

#include "table.h"

// enough keys to force several doublings of the initial capacity
#define MANY_KEYS 5000
#define KEY_SIZE 32

struct table {
  kvs_table_t table;
};

UTEST_F_SETUP(table) {
  kvs_table_init(&utest_fixture->table);
}

UTEST_F_TEARDOWN(table) {
  kvs_table_free(&utest_fixture->table);
}

// looks (key) up and compares the copy handed out with (expected),
// freeing the copy. a missing key is a failure.
#define EXPECT_STORED(table, key, expected)                                                        \
  do {                                                                                             \
    const char *found_ = NULL;                                                                     \
    ASSERT_TRUE(kvs_table_get((table), (key), &found_));                                           \
    ASSERT_TRUE(found_ != NULL);                                                                   \
    EXPECT_STREQ((expected), found_);                                                              \
    free((void *)found_);                                                                          \
  } while (0)

#define EXPECT_ABSENT(table, key)                                                                  \
  do {                                                                                             \
    const char *found_ = NULL;                                                                     \
    EXPECT_FALSE(kvs_table_get((table), (key), &found_));                                          \
  } while (0)

UTEST_F(table, get_finds_nothing_in_an_empty_table) {
  EXPECT_ABSENT(&utest_fixture->table, "a");
}

UTEST_F(table, delete_removes_nothing_from_an_empty_table) {
  EXPECT_FALSE(kvs_table_delete(&utest_fixture->table, "a"));
}

UTEST_F(table, free_of_an_empty_table_is_fine) {
  kvs_table_free(&utest_fixture->table);
  kvs_table_free(&utest_fixture->table);
}

UTEST_F(table, set_stores_a_value_get_returns_it) {
  ASSERT_TRUE(kvs_table_set(&utest_fixture->table, "alpha", "42"));
  EXPECT_STORED(&utest_fixture->table, "alpha", "42");
}

UTEST_F(table, set_of_an_existing_key_replaces_its_value) {
  ASSERT_TRUE(kvs_table_set(&utest_fixture->table, "alpha", "1"));
  ASSERT_TRUE(kvs_table_set(&utest_fixture->table, "alpha", "2"));
  EXPECT_STORED(&utest_fixture->table, "alpha", "2");
}

UTEST_F(table, keys_are_case_sensitive) {
  ASSERT_TRUE(kvs_table_set(&utest_fixture->table, "alpha", "1"));
  ASSERT_TRUE(kvs_table_set(&utest_fixture->table, "Alpha", "2"));
  EXPECT_STORED(&utest_fixture->table, "alpha", "1");
  EXPECT_STORED(&utest_fixture->table, "Alpha", "2");
}

UTEST_F(table, an_empty_value_is_stored_and_returned) {
  ASSERT_TRUE(kvs_table_set(&utest_fixture->table, "alpha", ""));
  EXPECT_STORED(&utest_fixture->table, "alpha", "");
}

UTEST_F(table, a_value_with_spaces_is_stored_whole) {
  ASSERT_TRUE(kvs_table_set(&utest_fixture->table, "alpha", "a b  c "));
  EXPECT_STORED(&utest_fixture->table, "alpha", "a b  c ");
}

UTEST_F(table, delete_reports_and_removes_a_present_key) {
  ASSERT_TRUE(kvs_table_set(&utest_fixture->table, "alpha", "1"));
  EXPECT_TRUE(kvs_table_delete(&utest_fixture->table, "alpha"));
  EXPECT_ABSENT(&utest_fixture->table, "alpha");
}

UTEST_F(table, delete_of_a_deleted_key_reports_absence) {
  ASSERT_TRUE(kvs_table_set(&utest_fixture->table, "alpha", "1"));
  ASSERT_TRUE(kvs_table_delete(&utest_fixture->table, "alpha"));
  EXPECT_FALSE(kvs_table_delete(&utest_fixture->table, "alpha"));
}

UTEST_F(table, a_deleted_key_can_be_set_again) {
  ASSERT_TRUE(kvs_table_set(&utest_fixture->table, "alpha", "1"));
  ASSERT_TRUE(kvs_table_delete(&utest_fixture->table, "alpha"));
  ASSERT_TRUE(kvs_table_set(&utest_fixture->table, "alpha", "2"));
  EXPECT_STORED(&utest_fixture->table, "alpha", "2");
}

// the copy get hands out is the caller's: it must survive every later
// operation on the table, including the ones that free what it was
// copied from.
UTEST_F(table, get_returns_a_copy_that_survives_overwrite_and_delete) {
  ASSERT_TRUE(kvs_table_set(&utest_fixture->table, "alpha", "first"));

  const char *copy = NULL;
  ASSERT_TRUE(kvs_table_get(&utest_fixture->table, "alpha", &copy));
  ASSERT_TRUE(copy != NULL);

  ASSERT_TRUE(kvs_table_set(&utest_fixture->table, "alpha", "second"));
  ASSERT_TRUE(kvs_table_delete(&utest_fixture->table, "alpha"));
  EXPECT_STREQ("first", copy);
  free((void *)copy);
}

// the caller's key and value are copied in, not kept: changing the
// caller's buffers afterwards must not change what is stored.
UTEST_F(table, set_copies_the_key_and_the_value) {
  char key[] = "alpha";
  char value[] = "42";
  ASSERT_TRUE(kvs_table_set(&utest_fixture->table, key, value));

  strcpy(key, "beta");
  strcpy(value, "99");
  EXPECT_STORED(&utest_fixture->table, "alpha", "42");
  EXPECT_ABSENT(&utest_fixture->table, "beta");
}

// keys differing only in their first byte must not all collide: with
// enough of them every lookup would walk one probe chain.
UTEST_F(table, keys_differing_in_the_first_byte_are_all_found) {
  char key[KEY_SIZE];
  for (int c = 'a'; c <= 'z'; c++) {
    snprintf(key, sizeof key, "%c-suffix", c);
    ASSERT_TRUE(kvs_table_set(&utest_fixture->table, key, key));
  }
  for (int c = 'a'; c <= 'z'; c++) {
    snprintf(key, sizeof key, "%c-suffix", c);
    EXPECT_STORED(&utest_fixture->table, key, key);
  }
}

UTEST_F(table, many_keys_survive_growth) {
  char key[KEY_SIZE];
  char value[KEY_SIZE];
  for (int i = 0; i < MANY_KEYS; i++) {
    snprintf(key, sizeof key, "key%d", i);
    snprintf(value, sizeof value, "value%d", i);
    ASSERT_TRUE(kvs_table_set(&utest_fixture->table, key, value));
  }
  for (int i = 0; i < MANY_KEYS; i++) {
    snprintf(key, sizeof key, "key%d", i);
    snprintf(value, sizeof value, "value%d", i);
    EXPECT_STORED(&utest_fixture->table, key, value);
  }
  EXPECT_ABSENT(&utest_fixture->table, "key-1");
  snprintf(key, sizeof key, "key%d", MANY_KEYS);
  EXPECT_ABSENT(&utest_fixture->table, key);
}

// removal must not break the lookup of any other key: deleting every
// other key leaves the rest findable, and the deleted ones absent.
UTEST_F(table, deleting_some_keys_leaves_the_others_findable) {
  char key[KEY_SIZE];
  for (int i = 0; i < MANY_KEYS; i++) {
    snprintf(key, sizeof key, "key%d", i);
    ASSERT_TRUE(kvs_table_set(&utest_fixture->table, key, key));
  }
  for (int i = 0; i < MANY_KEYS; i += 2) {
    snprintf(key, sizeof key, "key%d", i);
    ASSERT_TRUE(kvs_table_delete(&utest_fixture->table, key));
  }
  for (int i = 0; i < MANY_KEYS; i++) {
    snprintf(key, sizeof key, "key%d", i);
    if (i % 2 == 0) {
      EXPECT_ABSENT(&utest_fixture->table, key);
    } else {
      EXPECT_STORED(&utest_fixture->table, key, key);
    }
  }
}

// a set after deletes reuses the table without losing anything, and a
// grow that happens with tombstones present carries every live entry.
UTEST_F(table, growth_after_deletes_keeps_every_live_key) {
  char key[KEY_SIZE];
  for (int i = 0; i < MANY_KEYS; i++) {
    snprintf(key, sizeof key, "key%d", i);
    ASSERT_TRUE(kvs_table_set(&utest_fixture->table, key, key));
  }
  for (int i = 0; i < MANY_KEYS; i += 2) {
    snprintf(key, sizeof key, "key%d", i);
    ASSERT_TRUE(kvs_table_delete(&utest_fixture->table, key));
  }
  for (int i = MANY_KEYS; i < 2 * MANY_KEYS; i++) {
    snprintf(key, sizeof key, "key%d", i);
    ASSERT_TRUE(kvs_table_set(&utest_fixture->table, key, key));
  }
  for (int i = 1; i < 2 * MANY_KEYS; i += 2) {
    snprintf(key, sizeof key, "key%d", i);
    EXPECT_STORED(&utest_fixture->table, key, key);
  }
  for (int i = MANY_KEYS; i < 2 * MANY_KEYS; i += 2) {
    snprintf(key, sizeof key, "key%d", i);
    EXPECT_STORED(&utest_fixture->table, key, key);
  }
  for (int i = 0; i < MANY_KEYS; i += 2) {
    snprintf(key, sizeof key, "key%d", i);
    EXPECT_ABSENT(&utest_fixture->table, key);
  }
}

UTEST_F(table, free_releases_everything_and_leaves_an_empty_table) {
  char key[KEY_SIZE];
  for (int i = 0; i < 100; i++) {
    snprintf(key, sizeof key, "key%d", i);
    ASSERT_TRUE(kvs_table_set(&utest_fixture->table, key, key));
  }
  kvs_table_free(&utest_fixture->table);
  EXPECT_ABSENT(&utest_fixture->table, "key0");
  ASSERT_TRUE(kvs_table_set(&utest_fixture->table, "key0", "again"));
  EXPECT_STORED(&utest_fixture->table, "key0", "again");
}
