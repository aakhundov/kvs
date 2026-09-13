#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <unistd.h>

#include <utest.h>

#include "lines.h"

// The reader and writer over a pipe, which they cannot tell from a socket.
// Everything a test sends is written before the reader runs, so the cases
// come from what was written and where the reader's own buffer ends, not
// from timing: a line longer than KVS_READ_BUFFER_SIZE is the one the
// reader cannot take in a single read.

// what a test hands the reader: the protocol's request bound
#define LINE_CAP 1024

// long enough to span more than one of the reader's reads, short enough
// to sit whole in an unread pipe (65536 bytes measured here)
#define LONG_LINE ((2 * KVS_READ_BUFFER_SIZE) + 100)

struct lines {
  int read_end;
  int write_end;
  kvs_stream_t stream;
  char buf[LONG_LINE];
  size_t len;
};

UTEST_F_SETUP(lines) {
  int fds[2];
  ASSERT_EQ(0, pipe(fds));
  utest_fixture->read_end = fds[0];
  utest_fixture->write_end = fds[1];
  kvs_stream_init(&utest_fixture->stream, fds[0]);
  memset(utest_fixture->buf, 0, sizeof utest_fixture->buf);
  utest_fixture->len = 0;
}

UTEST_F_TEARDOWN(lines) {
  if (utest_fixture->write_end >= 0) {
    close(utest_fixture->write_end);
  }
  close(utest_fixture->read_end);
}

static void send_bytes(struct lines *f, const char *bytes, size_t n) {
  while (n > 0) {
    ssize_t written = write(f->write_end, bytes, n);
    if (written <= 0) {
      return;
    }
    bytes += written;
    n -= (size_t)written;
  }
}

static void send_text(struct lines *f, const char *text) {
  send_bytes(f, text, strlen(text));
}

static void end_input(struct lines *f) {
  close(f->write_end);
  f->write_end = -1;
}

// results come back as int so that ASSERT_EQ against an enumerator (an
// int in C) compares like with like
static int next(struct lines *f, size_t cap) {
  return (int)kvs_read_line(&f->stream, f->buf, cap, &f->len);
}

static int write_line(kvs_stream_t *out, const char *text, size_t n) {
  return (int)kvs_write_line(out, text, n);
}

// the line read is compared as bytes and length: it is not NUL-terminated
#define EXPECT_LINE(f, expected)                                                                   \
  do {                                                                                             \
    ASSERT_EQ(KVS_READ_SUCCESS, next((f), LINE_CAP));                                              \
    ASSERT_EQ(strlen(expected), (f)->len);                                                         \
    EXPECT_EQ(0, memcmp((expected), (f)->buf, (f)->len));                                          \
  } while (0)

UTEST_F(lines, a_line_arriving_whole) {
  send_text(utest_fixture, "SET a 1\n");
  EXPECT_LINE(utest_fixture, "SET a 1");
}

UTEST_F(lines, several_lines_in_one_read_come_out_in_order) {
  send_text(utest_fixture, "SET a 1\nGET a\nDEL a\n");
  EXPECT_LINE(utest_fixture, "SET a 1");
  EXPECT_LINE(utest_fixture, "GET a");
  EXPECT_LINE(utest_fixture, "DEL a");
}

UTEST_F(lines, an_empty_line_is_a_line) {
  send_text(utest_fixture, "\n");
  EXPECT_LINE(utest_fixture, "");
}

UTEST_F(lines, an_empty_line_between_others_is_a_line) {
  send_text(utest_fixture, "GET a\n\nGET b\n");
  EXPECT_LINE(utest_fixture, "GET a");
  EXPECT_LINE(utest_fixture, "");
  EXPECT_LINE(utest_fixture, "GET b");
}

UTEST_F(lines, end_of_input_with_nothing_pending_is_end) {
  end_input(utest_fixture);
  EXPECT_EQ(KVS_READ_END_OF_INPUT, next(utest_fixture, LINE_CAP));
}

UTEST_F(lines, end_of_input_is_permanent) {
  end_input(utest_fixture);
  EXPECT_EQ(KVS_READ_END_OF_INPUT, next(utest_fixture, LINE_CAP));
  EXPECT_EQ(KVS_READ_END_OF_INPUT, next(utest_fixture, LINE_CAP));
}

UTEST_F(lines, a_complete_final_line_comes_out_before_end) {
  send_text(utest_fixture, "GET a\n");
  end_input(utest_fixture);
  EXPECT_LINE(utest_fixture, "GET a");
  EXPECT_EQ(KVS_READ_END_OF_INPUT, next(utest_fixture, LINE_CAP));
}

UTEST_F(lines, a_partial_final_line_is_dropped_at_end) {
  send_text(utest_fixture, "GET a\nGET b");
  end_input(utest_fixture);
  EXPECT_LINE(utest_fixture, "GET a");
  EXPECT_EQ(KVS_READ_END_OF_INPUT, next(utest_fixture, LINE_CAP));
}

UTEST_F(lines, the_line_feed_is_stripped) {
  send_text(utest_fixture, "GET a\n");
  ASSERT_EQ(KVS_READ_SUCCESS, next(utest_fixture, LINE_CAP));
  ASSERT_EQ((size_t)5, utest_fixture->len);
  EXPECT_NE('\n', utest_fixture->buf[4]);
}

UTEST_F(lines, one_carriage_return_before_the_line_feed_is_stripped) {
  send_text(utest_fixture, "GET a\r\n");
  EXPECT_LINE(utest_fixture, "GET a");
}

UTEST_F(lines, every_carriage_return_before_the_line_feed_is_stripped) {
  send_text(utest_fixture, "GET a\r\r\r\n");
  EXPECT_LINE(utest_fixture, "GET a");
}

UTEST_F(lines, a_line_of_only_carriage_returns_is_an_empty_line) {
  send_text(utest_fixture, "\r\r\n");
  EXPECT_LINE(utest_fixture, "");
}

UTEST_F(lines, a_carriage_return_elsewhere_is_kept) {
  send_text(utest_fixture, "GET\ra\n");
  EXPECT_LINE(utest_fixture, "GET\ra");
}

UTEST_F(lines, a_carriage_return_before_a_kept_byte_is_kept) {
  send_text(utest_fixture, "GET a\r\rb\n");
  EXPECT_LINE(utest_fixture, "GET a\r\rb");
}

UTEST_F(lines, a_nul_byte_is_carried_like_any_other) {
  send_bytes(utest_fixture, "GE\0T\n", 5);
  ASSERT_EQ(KVS_READ_SUCCESS, next(utest_fixture, LINE_CAP));
  ASSERT_EQ((size_t)4, utest_fixture->len);
  EXPECT_EQ(0, memcmp("GE\0T", utest_fixture->buf, 4));
}

UTEST_F(lines, a_line_exactly_at_the_limit_is_a_line) {
  char line[LINE_CAP + 1];
  memset(line, 'x', LINE_CAP);
  line[LINE_CAP] = '\n';
  send_bytes(utest_fixture, line, sizeof line);
  ASSERT_EQ(KVS_READ_SUCCESS, next(utest_fixture, LINE_CAP));
  EXPECT_EQ((size_t)LINE_CAP, utest_fixture->len);
}

UTEST_F(lines, a_line_one_over_the_limit_is_too_long) {
  char line[LINE_CAP + 2];
  memset(line, 'x', LINE_CAP + 1);
  line[LINE_CAP + 1] = '\n';
  send_bytes(utest_fixture, line, sizeof line);
  EXPECT_EQ(KVS_READ_LINE_TOO_LONG, next(utest_fixture, LINE_CAP));
}

// the trailing carriage returns are not part of the line, so they do
// not count against the limit
UTEST_F(lines, carriage_returns_do_not_count_against_the_limit) {
  char line[LINE_CAP + 3];
  memset(line, 'x', LINE_CAP);
  line[LINE_CAP] = '\r';
  line[LINE_CAP + 1] = '\r';
  line[LINE_CAP + 2] = '\n';
  send_bytes(utest_fixture, line, sizeof line);
  ASSERT_EQ(KVS_READ_SUCCESS, next(utest_fixture, LINE_CAP));
  EXPECT_EQ((size_t)LINE_CAP, utest_fixture->len);
}

UTEST_F(lines, the_line_after_a_too_long_one_is_read_from_a_clean_start) {
  char line[LINE_CAP + 2];
  memset(line, 'x', LINE_CAP + 1);
  line[LINE_CAP + 1] = '\n';
  send_bytes(utest_fixture, line, sizeof line);
  send_text(utest_fixture, "GET a\n");
  ASSERT_EQ(KVS_READ_LINE_TOO_LONG, next(utest_fixture, LINE_CAP));
  EXPECT_LINE(utest_fixture, "GET a");
}

UTEST_F(lines, two_too_long_lines_in_a_row_are_each_reported) {
  char line[LINE_CAP + 2];
  memset(line, 'x', LINE_CAP + 1);
  line[LINE_CAP + 1] = '\n';
  send_bytes(utest_fixture, line, sizeof line);
  send_bytes(utest_fixture, line, sizeof line);
  send_text(utest_fixture, "GET a\n");
  ASSERT_EQ(KVS_READ_LINE_TOO_LONG, next(utest_fixture, LINE_CAP));
  ASSERT_EQ(KVS_READ_LINE_TOO_LONG, next(utest_fixture, LINE_CAP));
  EXPECT_LINE(utest_fixture, "GET a");
}

// longer than the reader's own buffer: the line has to be assembled
// across more than one read, which is the split-line case
UTEST_F(lines, a_line_longer_than_the_read_buffer_is_assembled) {
  char line[LONG_LINE + 1];
  for (size_t i = 0; i < LONG_LINE; i++) {
    line[i] = (char)('a' + (i % 26));
  }
  line[LONG_LINE] = '\n';
  send_bytes(utest_fixture, line, sizeof line);
  send_text(utest_fixture, "GET a\n");
  ASSERT_EQ(KVS_READ_SUCCESS, next(utest_fixture, LONG_LINE));
  ASSERT_EQ((size_t)LONG_LINE, utest_fixture->len);
  EXPECT_EQ(0, memcmp(line, utest_fixture->buf, LONG_LINE));
  EXPECT_LINE(utest_fixture, "GET a");
}

UTEST_F(lines, a_line_longer_than_the_read_buffer_and_the_limit_is_too_long) {
  char line[LONG_LINE + 1];
  memset(line, 'x', LONG_LINE);
  line[LONG_LINE] = '\n';
  send_bytes(utest_fixture, line, sizeof line);
  send_text(utest_fixture, "GET a\n");
  ASSERT_EQ(KVS_READ_LINE_TOO_LONG, next(utest_fixture, LINE_CAP));
  EXPECT_LINE(utest_fixture, "GET a");
}

UTEST_F(lines, a_too_long_line_cut_off_by_end_of_input_is_end) {
  char line[LINE_CAP + 1];
  memset(line, 'x', sizeof line);
  send_bytes(utest_fixture, line, sizeof line);
  end_input(utest_fixture);
  EXPECT_EQ(KVS_READ_END_OF_INPUT, next(utest_fixture, LINE_CAP));
}

UTEST_F(lines, a_too_long_line_beyond_the_read_buffer_cut_off_by_end_of_input_is_end) {
  char line[LONG_LINE];
  memset(line, 'x', sizeof line);
  send_bytes(utest_fixture, line, sizeof line);
  end_input(utest_fixture);
  EXPECT_EQ(KVS_READ_END_OF_INPUT, next(utest_fixture, LINE_CAP));
}

// a line that ends exactly where the read buffer does, and one that
// starts right after it, so the boundary itself is exercised
UTEST_F(lines, lines_around_the_read_buffer_boundary_come_out_whole) {
  char line[KVS_READ_BUFFER_SIZE];
  memset(line, 'x', sizeof line);
  line[KVS_READ_BUFFER_SIZE - 1] = '\n';
  send_bytes(utest_fixture, line, sizeof line);
  send_text(utest_fixture, "GET a\n");
  ASSERT_EQ(KVS_READ_SUCCESS, next(utest_fixture, KVS_READ_BUFFER_SIZE));
  EXPECT_EQ((size_t)(KVS_READ_BUFFER_SIZE - 1), utest_fixture->len);
  EXPECT_LINE(utest_fixture, "GET a");
}

UTEST_F(lines, the_written_line_ends_with_one_line_feed_and_nothing_else) {
  kvs_stream_t out;
  kvs_stream_init(&out, utest_fixture->write_end);
  ASSERT_EQ(KVS_WRITE_SUCCESS, write_line(&out, "VAL 42", 6));
  end_input(utest_fixture);

  char got[16];
  ssize_t n = read(utest_fixture->read_end, got, sizeof got);
  ASSERT_EQ((ssize_t)7, n);
  EXPECT_EQ(0, memcmp("VAL 42\n", got, 7));
  EXPECT_EQ((ssize_t)0, read(utest_fixture->read_end, got, sizeof got));
}

UTEST_F(lines, an_empty_line_is_written_as_a_line_feed) {
  kvs_stream_t out;
  kvs_stream_init(&out, utest_fixture->write_end);
  ASSERT_EQ(KVS_WRITE_SUCCESS, write_line(&out, "", 0));
  end_input(utest_fixture);

  char got[16];
  ASSERT_EQ((ssize_t)1, read(utest_fixture->read_end, got, sizeof got));
  EXPECT_EQ('\n', got[0]);
}

UTEST_F(lines, a_written_line_reads_back_through_the_reader) {
  kvs_stream_t out;
  kvs_stream_init(&out, utest_fixture->write_end);
  ASSERT_EQ(KVS_WRITE_SUCCESS, write_line(&out, "OK", 2));
  ASSERT_EQ(KVS_WRITE_SUCCESS, write_line(&out, "NIL", 3));
  EXPECT_LINE(utest_fixture, "OK");
  EXPECT_LINE(utest_fixture, "NIL");
}

UTEST_F(lines, writing_to_a_closed_read_end_reports_a_broken_connection) {
  close(utest_fixture->read_end);
  utest_fixture->read_end = -1;
  signal(SIGPIPE, SIG_IGN);

  kvs_stream_t out;
  kvs_stream_init(&out, utest_fixture->write_end);
  EXPECT_EQ(KVS_WRITE_CONNECTION_BROKEN, write_line(&out, "OK", 2));

  // the teardown closes the read end: give it a harmless one
  int fds[2];
  ASSERT_EQ(0, pipe(fds));
  close(fds[1]);
  utest_fixture->read_end = fds[0];
}
