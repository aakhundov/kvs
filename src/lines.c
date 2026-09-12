#include "lines.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <sys/uio.h>
#include <unistd.h>

#include "debug.h"

#define LOG_READ(...) KVS_LOG_WITH_ID("read", stream->fd, __VA_ARGS__)
#define LOG_WRITE(...) KVS_LOG_WITH_ID("write", stream->fd, __VA_ARGS__)
#define TRACE_READ(...) KVS_TRACE_WITH_ID("read", stream->fd, __VA_ARGS__)
#define TRACE_WRITE(...) KVS_TRACE_WITH_ID("write", stream->fd, __VA_ARGS__)

static void inline reset_stream(kvs_stream_t *stream) {
  stream->line_start = stream->cursor = stream->buffer;
  stream->read = true;
}

static kvs_read_result_t read_to_buffer(kvs_stream_t *stream) {
  while (true) {
    assert(stream->cursor >= stream->buffer);
    size_t n_previously_read = (size_t)(stream->cursor - stream->buffer);
    size_t n_to_read = sizeof(stream->buffer) - n_previously_read;

    errno = 0;
    ssize_t n_read = read(stream->fd, stream->cursor, n_to_read);

    if (n_read == 0) {
      TRACE_READ("end of input");
      return KVS_READ_END_OF_INPUT;
    }

    if (n_read < 0) {
      int error = errno;
      switch (error) {
      case EINTR:
        LOG_READ("interrupted");
        return KVS_READ_INTERRUPT;
      case EAGAIN: // case EWOULDBLOCK:
        TRACE_READ("retry");
        continue;
      case EPIPE:
      case ECONNRESET:
      case ETIMEDOUT:
      case ENETDOWN:
      case ENETUNREACH:
      case EHOSTUNREACH:
        LOG_READ("connection error: %s (%d)", strerror(error), error);
        return KVS_READ_CONNECTION_BROKEN;
      case ENOBUFS:
      case ENOMEM:
        LOG_READ("resource error: %s (%d)", strerror(error), error);
        return KVS_READ_OUT_OF_RESOURCE;
      default:
        LOG_READ("defect error: %s (%d)", strerror(error), error);
        return KVS_READ_DEFECT;
      }
    }

    TRACE_READ("%zd bytes", n_read);
    stream->cursor += n_read;
    return KVS_READ_SUCCESS;
  }
}

static kvs_read_result_t inline discard_line(kvs_stream_t *stream) {
  while (true) {
    // read the current line until \n is read
    reset_stream(stream);
    kvs_read_result_t result = read_to_buffer(stream);
    if (result != KVS_READ_SUCCESS) {
      return result;
    }

    for (char *c = stream->buffer; c < stream->cursor; c++) {
      if (*c == '\n') {
        TRACE_READ("line discarded");
        stream->line_start = c + 1; // next line
        stream->read = false;       // don't read yet
        return KVS_READ_SUCCESS;
      }
    }
  }
}

void kvs_stream_init(kvs_stream_t *stream, int fd) {
  stream->fd = fd;
  reset_stream(stream);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
kvs_read_result_t kvs_read_line(kvs_stream_t *stream, char *buf, size_t n, size_t *len) {
  assert(stream != NULL);
  assert(buf != NULL);
  assert(n > 0);
  assert(len != NULL);

  char *buf_pos = buf;
  size_t line_len = 0;
  char *scan_start = stream->line_start;

  while (true) {
    if (stream->read) {
      scan_start = stream->cursor;
      kvs_read_result_t result = read_to_buffer(stream);
      if (result != KVS_READ_SUCCESS) {
        if (result != KVS_READ_INTERRUPT) {
          reset_stream(stream);
        }
        return result;
      }
    }

    for (char *c = scan_start; c < stream->cursor; c++) {
      TRACE_READ("scan %d", *c);
      if (*c == '\n') {
        assert(c >= stream->line_start);
        size_t tail_len = (size_t)(c - stream->line_start); // drop \n

        // discard trailing \r chars from tail
        char *trailing;
        if (tail_len > 0) {
          trailing = c - 1;
          while (*trailing == '\r') { // NOLINT(clang-analyzer-security.ArrayBound)
            tail_len--;
            if (tail_len == 0) {
              break;
            }
            trailing--;
          }
        }
        // discard trailing \r chars from buf
        if (tail_len == 0 && line_len > 0) {
          trailing = buf_pos - 1;
          while (*trailing == '\r') {
            line_len--;
            buf_pos--;
            if (line_len == 0) {
              break;
            }
            trailing--;
          }
        }

        line_len += tail_len;

        if (line_len > n) {
          LOG_READ("error: line too long (finished)");
          stream->line_start = c + 1; // next line
          stream->read = false;       // don't read yet
          return KVS_READ_LINE_TOO_LONG;
        }

        if (tail_len > 0) {
          // copy the line tail to buf
          TRACE_READ("tail \"%.*s\"", (int)tail_len, stream->line_start);
          memcpy(buf_pos, stream->line_start, tail_len);
        }
        stream->line_start = c + 1; // next line
        stream->read = false;       // don't read yet

        LOG_READ("line \"%.*s\"", (int)line_len, buf);
        *len = line_len;
        return KVS_READ_SUCCESS;
      }
    }

    stream->read = true; // no \n found: read further

    char *buffer_end = stream->buffer + sizeof(stream->buffer);
    if (stream->cursor >= buffer_end) {
      assert(stream->line_start <= buffer_end);
      size_t chunk_len = (size_t)(buffer_end - stream->line_start);

      line_len += chunk_len;
      if (line_len > n) {
        kvs_read_result_t result = discard_line(stream);
        if (result != KVS_READ_SUCCESS) {
          reset_stream(stream);
          return result; // supersede outer error
        }
        LOG_READ("error: line too long (unfinished)");
        return KVS_READ_LINE_TOO_LONG;
      }

      // copy the line chunk to buf
      TRACE_READ("chunk \"%.*s\"", (int)chunk_len, stream->line_start);
      memcpy(buf_pos, stream->line_start, chunk_len);
      buf_pos += chunk_len;
      reset_stream(stream);
    }
  }
}

kvs_write_result_t kvs_write_line(kvs_stream_t *stream, const char *buf, size_t n) {
  // a vector with buf and \n appended to it
  struct iovec iov[] = {
      {.iov_base = (void *)buf, .iov_len = n},
      {.iov_base = (void *)"\n", .iov_len = 1},
  };

  size_t n_to_write = n + 1; // include \n
  while (n_to_write > 0) {
    errno = 0;
    ssize_t n_written = writev(stream->fd, iov, 2);

    if (n_written < 0) {
      int error = errno;
      switch (error) {
      case EINTR:
        LOG_WRITE("interrupted");
        return KVS_WRITE_INTERRUPT;
      case EAGAIN: // case EWOULDBLOCK:
        TRACE_WRITE("retry");
        continue;
      case EPIPE:
      case ECONNRESET:
      case ETIMEDOUT:
      case ENETDOWN:
      case ENETUNREACH:
      case EHOSTUNREACH:
        LOG_WRITE("connection error: %s (%d)", strerror(error), error);
        return KVS_WRITE_CONNECTION_BROKEN;
      case ENOBUFS:
      case ENOMEM:
        LOG_WRITE("resource error: %s (%d)", strerror(error), error);
        return KVS_WRITE_OUT_OF_RESOURCE;
      default:
        LOG_WRITE("defect error: %s (%d)", strerror(error), error);
        return KVS_WRITE_DEFECT;
      }
    }

    assert(n_written != 0);
    assert((size_t)n_written <= n_to_write);
    TRACE_WRITE("%zd bytes", n_written);

    n_to_write -= (size_t)n_written;
    if (n_to_write > 0) {
      // move the first vector by the written part
      iov[0].iov_base = (void *)((char *)iov[0].iov_base + n_written);
      iov[0].iov_len -= (size_t)n_written;
    }
  }

  LOG_WRITE("line \"%.*s\"", (int)n, buf);
  return KVS_WRITE_SUCCESS;
}
