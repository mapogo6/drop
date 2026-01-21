#include <drop/tftp.h>

#include <arpa/inet.h>
#include <asm-generic/errno.h>
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>

typedef struct {
  uint8_t *begin;
  uint8_t *end;
} tftp_buffer_view_t;

static tftp_buffer_view_t tftp_buffer_reader(tftp_buffer_t *buffer,
                                             size_t size);
static tftp_buffer_view_t tftp_buffer_writer(tftp_buffer_t *buffer);

typedef struct {
  bool has_value;
  union {
    uint16_t value;
    int error;
  };
} expected_uint16_t;

typedef struct {
  bool has_value;
  union {
    const char *value;
    int error;
  };
} expected_string_t;

tftp_buffer_view_t tftp_buffer_reader(tftp_buffer_t *buffer, size_t size) {
  return (tftp_buffer_view_t){
      .begin = buffer->buffer,
      .end = buffer->buffer + size,
  };
}

tftp_buffer_view_t tftp_buffer_writer(tftp_buffer_t *buffer) {
  return tftp_buffer_reader(buffer, sizeof(buffer->buffer));
}

static size_t tftp_buffer_view_capacity(tftp_buffer_view_t *view) {
  return view->end - view->begin;
}

static expected_uint16_t tftp_buffer_read_uint16_t(tftp_buffer_view_t *reader) {
  if (tftp_buffer_view_capacity(reader) < sizeof(uint16_t))
    return (expected_uint16_t){
        .has_value = false,
        .error = EMSGSIZE,
    };

  uint16_t result;
  memcpy(&result, reader->begin, sizeof(uint16_t));

  reader->begin += sizeof(uint16_t);

  return (expected_uint16_t){
      .has_value = true,
      .value = ntohs(result),
  };
}

static expected_string_t tftp_buffer_read_string(tftp_buffer_view_t *reader) {
  const char *string = (const char *)reader->begin;
  const size_t max = tftp_buffer_view_capacity(reader);
  const size_t length = strnlen(string, max);

  if (length == max)
    return (expected_string_t){
        .has_value = false,
        .error = EBADMSG,
    };

  reader->begin += length + 1;

  return (expected_string_t){
      .has_value = true,
      .value = string,
  };
}

typedef struct {
  bool has_value;
  union {
    size_t value;
    int error;
  };
} expected_size_t;

static expected_size_t tftp_buffer_write_data(tftp_buffer_view_t *writer,
                                              const uint8_t *data,
                                              size_t size) {
  if (tftp_buffer_view_capacity(writer) < size)
    return (expected_size_t){
        .has_value = false,
        .error = EMSGSIZE,
    };

  memcpy(writer->begin, data, size);
  writer->begin += size;

  return (expected_size_t){
      .has_value = true,
      .value = size,
  };
}

static expected_size_t tftp_buffer_write_uint16_t(tftp_buffer_view_t *writer,
                                                  uint16_t value) {
  value = htons(value);
  return tftp_buffer_write_data(writer, (const uint8_t *)&value, sizeof(value));
}

static expected_size_t tftp_buffer_write_string(tftp_buffer_view_t *writer,
                                                const char *value) {
  return tftp_buffer_write_data(writer, (const uint8_t *)value,
                                strlen(value) + 1);
}

typedef struct {
  bool has_value;
  union {
    tftp_read_request_t value;
    int error;
  };
} expected_tftp_read_request_t;

static expected_tftp_read_request_t
tftp_parse_read_request(tftp_buffer_view_t *reader) {
  expected_string_t filename = tftp_buffer_read_string(reader);
  if (!filename.has_value)
    return (expected_tftp_read_request_t){
        .has_value = false,
        .error = filename.error,
    };

  expected_string_t mode = tftp_buffer_read_string(reader);
  if (!mode.has_value)
    return (expected_tftp_read_request_t){
        .has_value = false,
        .error = mode.error,
    };

  return (expected_tftp_read_request_t){
      .has_value = true,
      .value =
          {
              .filename = filename.value,
              .mode = mode.value,
          },
  };
}

typedef struct {
  bool has_value;
  union {
    tftp_write_request_t value;
    int error;
  };
} expected_tftp_write_request_t;

static expected_tftp_write_request_t
tftp_parse_write_request(tftp_buffer_view_t *writer) {
  expected_string_t filename = tftp_buffer_read_string(writer);
  if (!filename.has_value)
    return (expected_tftp_write_request_t){
        .has_value = false,
        .error = filename.error,
    };

  expected_string_t mode = tftp_buffer_read_string(writer);
  if (!mode.has_value)
    return (expected_tftp_write_request_t){
        .has_value = false,
        .error = mode.error,
    };

  return (expected_tftp_write_request_t){
      .has_value = true,
      .value =
          {
              .filename = filename.value,
              .mode = mode.value,
          },
  };
}

typedef struct {
  bool has_value;
  union {
    tftp_data_t value;
    int error;
  };
} expected_tftp_data_t;

static expected_tftp_data_t tftp_parse_data(tftp_buffer_view_t *reader) {
  expected_uint16_t block = tftp_buffer_read_uint16_t(reader);
  if (!block.has_value)
    return (expected_tftp_data_t){
        .has_value = false,
        .error = block.error,
    };

  return (expected_tftp_data_t){
      .has_value = true,
      .value =
          {
              .block = block.value,
              .data = reader->begin,
              .size = tftp_buffer_view_capacity(reader),
          },
  };
}

typedef struct {
  bool has_value;
  union {
    tftp_ack_t value;
    int error;
  };
} expected_tftp_ack_t;

static expected_tftp_ack_t tftp_parse_ack(tftp_buffer_view_t *reader) {
  expected_uint16_t block = tftp_buffer_read_uint16_t(reader);
  if (!block.has_value)
    return (expected_tftp_ack_t){
        .has_value = false,
        .error = block.error,
    };

  return (expected_tftp_ack_t){
      .has_value = true,
      .value =
          {
              .block = block.value,
          },
  };
}

typedef struct {
  bool has_value;
  union {
    tftp_error_t value;
    int error;
  };
} expected_tftp_error_t;

static expected_tftp_error_t tftp_parse_error(tftp_buffer_view_t *reader) {
  expected_uint16_t code = tftp_buffer_read_uint16_t(reader);
  if (!code.has_value)
    return (expected_tftp_error_t){
        .has_value = false,
        .error = code.error,
    };

  expected_string_t message = tftp_buffer_read_string(reader);
  if (!message.has_value)
    return (expected_tftp_error_t){
        .has_value = false,
        .error = message.error,
    };

  return (expected_tftp_error_t){
      .has_value = true,
      .value =
          {
              .code = code.value,
              .message = message.value,
          },
  };
};

bool tftp_parse(tftp_buffer_t *buffer, size_t size, tftp_packet_t *out) {
  tftp_buffer_view_t reader = tftp_buffer_reader(buffer, size);
  expected_uint16_t opcode = tftp_buffer_read_uint16_t(&reader);
  if (!opcode.has_value) {
    errno = EBADMSG;
    return false;
  }

  out->opcode = opcode.value;

  switch (out->opcode) {
  case TFTP_OPCODE_RRQ: {
    expected_tftp_read_request_t rrq = tftp_parse_read_request(&reader);
    if (!rrq.has_value) {
      errno = rrq.error;
      return false;
    }
    memcpy(&out->rrq, &rrq.value, sizeof(tftp_read_request_t));
    return true;
  }
  case TFTP_OPCODE_WRQ: {
    expected_tftp_write_request_t wrq = tftp_parse_write_request(&reader);
    if (!wrq.has_value) {
      errno = wrq.error;
      return false;
    }
    memcpy(&out->wrq, &wrq.value, sizeof(tftp_write_request_t));
    return true;
  }
  case TFTP_OPCODE_DATA: {
    expected_tftp_data_t data = tftp_parse_data(&reader);
    if (!data.has_value) {
      errno = data.error;
      return false;
    }
    memcpy(&out->data, &data.value, sizeof(tftp_data_t));
    return true;
  }
  case TFTP_OPCODE_ACK: {
    expected_tftp_ack_t ack = tftp_parse_ack(&reader);
    if (!ack.has_value) {
      errno = ack.error;
      return false;
    }
    memcpy(&out->ack, &ack.value, sizeof(tftp_ack_t));
    return true;
  }
  case TFTP_OPCODE_ERROR: {
    expected_tftp_error_t error = tftp_parse_error(&reader);
    if (!error.has_value) {
      errno = error.error;
      return false;
    }
    memcpy(&out->error, &error.value, sizeof(tftp_error_t));
    return true;
  }
  default:
    errno = EBADMSG;
    return false;
  }
}

ssize_t tftp_new_rrq(tftp_buffer_t *buffer, const char *in_filename,
                     const char *in_mode) {
  tftp_buffer_view_t writer = tftp_buffer_writer(buffer);
  expected_size_t opcode = tftp_buffer_write_uint16_t(&writer, TFTP_OPCODE_RRQ);
  if (!opcode.has_value) {
    errno = opcode.error;
    return -1;
  }

  expected_size_t filename = tftp_buffer_write_string(&writer, in_filename);
  if (!filename.has_value) {
    errno = filename.error;
    return -1;
  }

  expected_size_t mode = tftp_buffer_write_string(&writer, in_mode);
  if (!mode.has_value) {
    errno = mode.error;
    return -1;
  }

  return opcode.value + filename.value + mode.value;
}

ssize_t tftp_new_wrq(tftp_buffer_t *buffer, const char *in_filename,
                     const char *in_mode) {
  tftp_buffer_view_t writer = tftp_buffer_writer(buffer);
  expected_size_t opcode = tftp_buffer_write_uint16_t(&writer, TFTP_OPCODE_WRQ);
  if (!opcode.has_value) {
    errno = opcode.error;
    return -1;
  }

  expected_size_t filename = tftp_buffer_write_string(&writer, in_filename);
  if (!filename.has_value) {
    errno = filename.error;
    return -1;
  }

  expected_size_t mode = tftp_buffer_write_string(&writer, in_mode);
  if (!mode.has_value) {
    errno = mode.error;
    return -1;
  }

  return opcode.value + filename.value + mode.value;
}

ssize_t tftp_new_data(tftp_buffer_t *buffer, tftp_block_t in_block,
                      const uint8_t *in_data, size_t in_data_size) {
  tftp_buffer_view_t writer = tftp_buffer_writer(buffer);
  expected_size_t opcode =
      tftp_buffer_write_uint16_t(&writer, TFTP_OPCODE_DATA);
  if (!opcode.has_value) {
    errno = opcode.error;
    return -1;
  }

  expected_size_t block = tftp_buffer_write_uint16_t(&writer, in_block);
  if (!block.has_value) {
    errno = block.error;
    return -1;
  }

  expected_size_t data = tftp_buffer_write_data(&writer, in_data, in_data_size);
  if (!data.has_value) {
    errno = data.error;
    return -1;
  }

  return opcode.value + block.value + data.value;
}

ssize_t tftp_new_ack(tftp_buffer_t *buffer, tftp_block_t in_block) {
  tftp_buffer_view_t writer = tftp_buffer_writer(buffer);
  expected_size_t opcode = tftp_buffer_write_uint16_t(&writer, TFTP_OPCODE_ACK);
  if (!opcode.has_value) {
    errno = opcode.error;
    return -1;
  }

  expected_size_t block = tftp_buffer_write_uint16_t(&writer, in_block);
  if (!block.has_value) {
    errno = block.error;
    return -1;
  }

  return opcode.value + block.value;
}

ssize_t tftp_new_error(tftp_buffer_t *buffer, tftp_error_code_t code,
                       const char *message) {
  tftp_buffer_view_t writer = tftp_buffer_writer(buffer);

  expected_size_t opcode =
      tftp_buffer_write_uint16_t(&writer, TFTP_OPCODE_ERROR);
  if (!opcode.has_value) {
    errno = opcode.error;
    return -1;
  }

  expected_size_t error = tftp_buffer_write_uint16_t(&writer, code);
  if (!error.has_value) {
    errno = error.error;
    return -1;
  }

  expected_size_t msg = tftp_buffer_write_string(&writer, message);
  if (!msg.has_value) {
    errno = msg.error;
    return -1;
  }

  return opcode.value + error.value + msg.value;
}
