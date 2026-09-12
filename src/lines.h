#ifndef KVS_LINES_H
#define KVS_LINES_H

#include <stdbool.h>
#include <stddef.h>

#define KVS_READ_BUFFER_SIZE 4096

typedef struct kvs_stream_t {
  int fd;
  char buffer[KVS_READ_BUFFER_SIZE];
  char *line_start;
  char *cursor;
  bool read;
} kvs_stream_t;

typedef enum kvs_read_result_t {
  KVS_READ_SUCCESS,           // successful read
  KVS_READ_INTERRUPT,         // interrupted by signal
  KVS_READ_END_OF_INPUT,      // descriptor closed
  KVS_READ_LINE_TOO_LONG,     // line longer than (n)
  KVS_READ_CONNECTION_BROKEN, // connection issue
  KVS_READ_OUT_OF_RESOURCE,   // resource issue
  KVS_READ_DEFECT,            // program defect: unexpected
} kvs_read_result_t;

typedef enum kvs_write_result_t {
  KVS_WRITE_SUCCESS,           // successful write
  KVS_WRITE_INTERRUPT,         // interrupted by signal
  KVS_WRITE_CONNECTION_BROKEN, // connection issue
  KVS_WRITE_OUT_OF_RESOURCE,   // resource issue
  KVS_WRITE_DEFECT,            // program defect: unexpected
} kvs_write_result_t;

void kvs_stream_init(kvs_stream_t *stream, int fd);

// fills in the (buffer) up to (n) chars with a line read from the stream
// (without trailing \r or \n chars). if READ_SUCCESS is returned, the
// length of the read line is written to (len).
kvs_read_result_t kvs_read_line(kvs_stream_t *stream, char *buffer, size_t n, size_t *len);

// writes the (n) chars from (buffer) to the stream as a line. the (n) chars
// of (buffer) should not contain trailing \r, \n or NUL: trailing \n is written
// by the function itself, immedeately after the (n) chars of (buffer).
kvs_write_result_t kvs_write_line(kvs_stream_t *stream, const char *buffer, size_t n);

#endif
