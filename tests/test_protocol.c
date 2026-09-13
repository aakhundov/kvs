#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <utest.h>

#include "error.h"
#include "protocol.h"
#include "server.h"
#include "table.h"

// The parser on strings, with no descriptor anywhere: a request in, a reply
// out, through a real table. Only the ERR prefix is asserted on an error,
// since the reason after it is the server's choice and not the contract's.

struct protocol {
  kvs_table_t table;
  char request[KVS_MAX_LINE_LENGTH + 1];
  char response[KVS_MAX_LINE_LENGTH + 1];
};

UTEST_F_SETUP(protocol) {
  kvs_table_init(&utest_fixture->table);
  memset(utest_fixture->request, 0, sizeof utest_fixture->request);
  memset(utest_fixture->response, 0, sizeof utest_fixture->response);
}

UTEST_F_TEARDOWN(protocol) {
  kvs_table_free(&utest_fixture->table);
}

// the parser may write into the request within its bounds, so every
// case hands it a fresh copy
static const char *handle(struct protocol *f, const char *request) {
  snprintf(f->request, sizeof f->request, "%s", request);
  memset(f->response, '#', sizeof f->response); // any leftover is visible
  kvs_protocol_handle(&f->table, f->request, f->response);
  return f->response;
}

#define EXPECT_REPLY(f, request, expected) EXPECT_STREQ((expected), handle((f), (request)))

#define EXPECT_ERR(f, request)                                                                     \
  do {                                                                                             \
    const char *reply_ = handle((f), (request));                                                   \
    EXPECT_EQ(0, strncmp(KVS_ERROR_PREFIX, reply_, strlen(KVS_ERROR_PREFIX)));                     \
    EXPECT_LT(strlen(KVS_ERROR_PREFIX), strlen(reply_));                                           \
  } while (0)

UTEST_F(protocol, set_replies_ok) {
  EXPECT_REPLY(utest_fixture, "SET alpha 42", "OK");
}

UTEST_F(protocol, get_of_a_set_key_replies_its_value) {
  EXPECT_REPLY(utest_fixture, "SET alpha 42", "OK");
  EXPECT_REPLY(utest_fixture, "GET alpha", "VAL 42");
}

UTEST_F(protocol, get_of_a_missing_key_replies_nil) {
  EXPECT_REPLY(utest_fixture, "GET missing", "NIL");
}

UTEST_F(protocol, get_of_a_missing_key_in_a_filled_table_replies_nil) {
  EXPECT_REPLY(utest_fixture, "SET alpha 42", "OK");
  EXPECT_REPLY(utest_fixture, "GET beta", "NIL");
}

UTEST_F(protocol, set_of_an_existing_key_overwrites) {
  EXPECT_REPLY(utest_fixture, "SET alpha 1", "OK");
  EXPECT_REPLY(utest_fixture, "SET alpha 2", "OK");
  EXPECT_REPLY(utest_fixture, "GET alpha", "VAL 2");
}

UTEST_F(protocol, del_of_a_present_key_replies_ok_and_removes_it) {
  EXPECT_REPLY(utest_fixture, "SET alpha 42", "OK");
  EXPECT_REPLY(utest_fixture, "DEL alpha", "OK");
  EXPECT_REPLY(utest_fixture, "GET alpha", "NIL");
}

UTEST_F(protocol, del_of_a_missing_key_replies_nil) {
  EXPECT_REPLY(utest_fixture, "DEL missing", "NIL");
}

UTEST_F(protocol, del_twice_replies_ok_then_nil) {
  EXPECT_REPLY(utest_fixture, "SET alpha 42", "OK");
  EXPECT_REPLY(utest_fixture, "DEL alpha", "OK");
  EXPECT_REPLY(utest_fixture, "DEL alpha", "NIL");
}

UTEST_F(protocol, keys_are_case_sensitive) {
  EXPECT_REPLY(utest_fixture, "SET alpha 1", "OK");
  EXPECT_REPLY(utest_fixture, "GET Alpha", "NIL");
  EXPECT_REPLY(utest_fixture, "GET ALPHA", "NIL");
  EXPECT_REPLY(utest_fixture, "GET alpha", "VAL 1");
}

UTEST_F(protocol, a_value_may_contain_spaces_and_is_kept_whole) {
  EXPECT_REPLY(utest_fixture, "SET k a b  c ", "OK");
  EXPECT_REPLY(utest_fixture, "GET k", "VAL a b  c ");
}

UTEST_F(protocol, a_value_may_be_empty) {
  EXPECT_REPLY(utest_fixture, "SET k ", "OK");
  EXPECT_REPLY(utest_fixture, "GET k", "VAL ");
}

UTEST_F(protocol, a_second_space_after_the_key_begins_the_value) {
  EXPECT_REPLY(utest_fixture, "SET k  v", "OK");
  EXPECT_REPLY(utest_fixture, "GET k", "VAL  v");
}

UTEST_F(protocol, a_tab_is_data) {
  EXPECT_REPLY(utest_fixture, "SET k\tx 1", "OK");
  EXPECT_REPLY(utest_fixture, "GET k\tx", "VAL 1");
  EXPECT_REPLY(utest_fixture, "GET k", "NIL");
  EXPECT_REPLY(utest_fixture, "SET k \t", "OK");
  EXPECT_REPLY(utest_fixture, "GET k", "VAL \t");
}

UTEST_F(protocol, an_empty_request_is_an_error) {
  EXPECT_ERR(utest_fixture, "");
}

UTEST_F(protocol, an_unknown_command_is_an_error) {
  EXPECT_ERR(utest_fixture, "PUT a 1");
  EXPECT_ERR(utest_fixture, "GETS a");
  EXPECT_ERR(utest_fixture, "GE a");
}

UTEST_F(protocol, commands_are_uppercase_only) {
  EXPECT_REPLY(utest_fixture, "SET a 1", "OK");
  EXPECT_ERR(utest_fixture, "get a");
  EXPECT_ERR(utest_fixture, "Get a");
  EXPECT_ERR(utest_fixture, "set a 1");
  EXPECT_ERR(utest_fixture, "del a");
}

UTEST_F(protocol, a_command_alone_is_an_error) {
  EXPECT_ERR(utest_fixture, "GET");
  EXPECT_ERR(utest_fixture, "SET");
  EXPECT_ERR(utest_fixture, "DEL");
}

UTEST_F(protocol, a_missing_key_is_an_error) {
  EXPECT_ERR(utest_fixture, "GET ");
  EXPECT_ERR(utest_fixture, "SET ");
  EXPECT_ERR(utest_fixture, "DEL ");
  EXPECT_ERR(utest_fixture, "SET  1");
}

UTEST_F(protocol, a_space_before_the_command_is_an_error) {
  EXPECT_ERR(utest_fixture, " GET a");
  EXPECT_ERR(utest_fixture, " SET a 1");
}

UTEST_F(protocol, a_doubled_space_after_the_command_is_an_error) {
  EXPECT_ERR(utest_fixture, "GET  a");
  EXPECT_ERR(utest_fixture, "SET  a 1");
  EXPECT_ERR(utest_fixture, "DEL  a");
}

UTEST_F(protocol, a_space_in_a_get_or_del_key_is_an_error) {
  EXPECT_REPLY(utest_fixture, "SET a 1", "OK");
  EXPECT_ERR(utest_fixture, "GET a b");
  EXPECT_ERR(utest_fixture, "GET a ");
  EXPECT_ERR(utest_fixture, "DEL a b");
  EXPECT_ERR(utest_fixture, "DEL a ");
  EXPECT_REPLY(utest_fixture, "GET a", "VAL 1");
}

UTEST_F(protocol, set_without_a_value_is_an_error) {
  EXPECT_ERR(utest_fixture, "SET a");
  EXPECT_REPLY(utest_fixture, "GET a", "NIL");
}

UTEST_F(protocol, an_error_leaves_the_store_untouched) {
  EXPECT_REPLY(utest_fixture, "SET a 1", "OK");
  EXPECT_ERR(utest_fixture, "SET a");
  EXPECT_ERR(utest_fixture, "DEL a ");
  EXPECT_REPLY(utest_fixture, "GET a", "VAL 1");
}

UTEST_F(protocol, a_request_is_not_kept_by_the_store) {
  EXPECT_REPLY(utest_fixture, "SET alpha 42", "OK");
  memset(utest_fixture->request, 'z', KVS_MAX_LINE_LENGTH);
  EXPECT_REPLY(utest_fixture, "GET alpha", "VAL 42");
}

// the longest request the contract allows, and the longest reply it
// can produce from one: both must fit the bound
UTEST_F(protocol, the_longest_value_comes_back_within_the_reply_bound) {
  char request[KVS_MAX_LINE_LENGTH + 1];
  const char *head = "SET k ";
  size_t head_len = strlen(head);
  memcpy(request, head, head_len);
  memset(request + head_len, 'v', KVS_MAX_LINE_LENGTH - head_len);
  request[KVS_MAX_LINE_LENGTH] = '\0';
  ASSERT_EQ((size_t)KVS_MAX_LINE_LENGTH, strlen(request));

  EXPECT_REPLY(utest_fixture, request, "OK");

  const char *reply = handle(utest_fixture, "GET k");
  ASSERT_EQ(0, strncmp("VAL ", reply, 4));
  EXPECT_EQ(KVS_MAX_LINE_LENGTH - head_len, strlen(reply + 4));
  EXPECT_LE(strlen(reply), (size_t)KVS_MAX_LINE_LENGTH);
  EXPECT_EQ(0, strcmp(request + head_len, reply + 4));
}

UTEST_F(protocol, the_longest_key_is_accepted) {
  char request[KVS_MAX_LINE_LENGTH + 1];
  memcpy(request, "SET ", 4);
  memset(request + 4, 'k', KVS_MAX_LINE_LENGTH - 6);
  memcpy(request + KVS_MAX_LINE_LENGTH - 2, " 1", 3);
  ASSERT_EQ((size_t)KVS_MAX_LINE_LENGTH, strlen(request));
  EXPECT_REPLY(utest_fixture, request, "OK");

  memcpy(request, "GET ", 4);
  request[KVS_MAX_LINE_LENGTH - 2] = '\0';
  EXPECT_REPLY(utest_fixture, request, "VAL 1");
}

UTEST_F(protocol, every_error_text_fits_the_reply_bound) {
  for (int e = 0; e < KVS_ERROR_COUNT; e++) {
    const char *text = kvs_error_texts[e];
    ASSERT_TRUE(text != NULL);
    EXPECT_EQ(0, strncmp(KVS_ERROR_PREFIX, text, strlen(KVS_ERROR_PREFIX)));
    EXPECT_LE(strlen(text), (size_t)KVS_MAX_LINE_LENGTH);
  }
}
