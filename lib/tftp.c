#include <drop/tftp.h>

#include <arpa/inet.h>
#include <asm-generic/errno.h>
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>

#define TFTP_TIMEOUT 5

typedef enum tftp_opcode : uint16_t {
  TFTP_OPCODE_RRQ = 1,
  TFTP_OPCODE_WRQ = 2,
  TFTP_OPCODE_DATA = 3,
  TFTP_OPCODE_ACK = 4,
  TFTP_OPCODE_ERROR = 5,
} tftp_opcode_t;

typedef struct {
  const char *filename;
  const char *mode;
} tftp_rrq_t;

typedef struct {
  const char *filename;
  const char *mode;
} tftp_wrq_t;

typedef uint16_t tftp_block_t;

typedef struct {
  tftp_block_t block;
  const uint8_t *data;
  size_t size;
} tftp_data_t;

typedef struct {
  tftp_block_t block;
} tftp_ack_t;

typedef enum tftp_error_code : uint16_t {
  TFTP_ERROR_NOT_DEFINED = 0,
  TFTP_ERROR_NOT_FOUND = 1,
  TFTP_ERROR_ACCESS_VIOLATION = 2,
  TFTP_ERROR_DISK_FULL = 3,
  TFTP_ERROR_ILLEGAL_OPERATION = 4,
  TFTP_ERROR_UNKNOWN_TID = 5,
  TFTP_ERROR_ALREADY_EXISTS = 6,
  TFTP_ERROR_NO_SUCH_USER = 7
} tftp_error_code_t;

typedef struct {
  tftp_error_code_t code;
  const char *message;
} tftp_error_t;

typedef struct {
  tftp_opcode_t opcode;
  union {
    tftp_rrq_t rrq;
    tftp_wrq_t wrq;
    tftp_data_t data;
    tftp_ack_t ack;
    tftp_error_t error;
  };
} tftp_packet_t;

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

static tftp_buffer_view_t tftp_buffer_reader(tftp_buffer_t *buffer,
                                             size_t size) {
  return (tftp_buffer_view_t){
      .begin = buffer->buffer,
      .end = buffer->buffer + size,
  };
}

static tftp_buffer_view_t tftp_buffer_writer(tftp_buffer_t *buffer) {
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
    tftp_rrq_t value;
    int error;
  };
} expected_tftp_rrq_t;

static expected_tftp_rrq_t tftp_read_rrq(tftp_buffer_view_t *reader) {
  expected_string_t filename = tftp_buffer_read_string(reader);
  if (!filename.has_value)
    return (expected_tftp_rrq_t){
        .has_value = false,
        .error = filename.error,
    };

  expected_string_t mode = tftp_buffer_read_string(reader);
  if (!mode.has_value)
    return (expected_tftp_rrq_t){
        .has_value = false,
        .error = mode.error,
    };

  return (expected_tftp_rrq_t){
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
    tftp_wrq_t value;
    int error;
  };
} expected_tftp_wrq_t;

static expected_tftp_wrq_t tftp_read_wrq(tftp_buffer_view_t *writer) {
  expected_string_t filename = tftp_buffer_read_string(writer);
  if (!filename.has_value)
    return (expected_tftp_wrq_t){
        .has_value = false,
        .error = filename.error,
    };

  expected_string_t mode = tftp_buffer_read_string(writer);
  if (!mode.has_value)
    return (expected_tftp_wrq_t){
        .has_value = false,
        .error = mode.error,
    };

  return (expected_tftp_wrq_t){
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

static expected_tftp_data_t tftp_read_data(tftp_buffer_view_t *reader) {
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

static expected_tftp_ack_t tftp_read_ack(tftp_buffer_view_t *reader) {
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

static expected_tftp_error_t tftp_read_error(tftp_buffer_view_t *reader) {
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

typedef struct {
  bool has_value;
  union {
    tftp_packet_t value;
    int error;
  };
} expected_tftp_packet_t;

static expected_tftp_packet_t tftp_parse(tftp_buffer_t *buffer, size_t size) {
  tftp_buffer_view_t reader = tftp_buffer_reader(buffer, size);
  expected_uint16_t opcode = tftp_buffer_read_uint16_t(&reader);
  if (!opcode.has_value)
    return (expected_tftp_packet_t){
        .has_value = false,
        .error = opcode.error,
    };

  switch (opcode.value) {
  case TFTP_OPCODE_RRQ: {
    expected_tftp_rrq_t rrq = tftp_read_rrq(&reader);
    if (!rrq.has_value)
      return (expected_tftp_packet_t){
          .has_value = false,
          .error = rrq.error,
      };

    return (expected_tftp_packet_t){
        .has_value = true,
        .value =
            {
                .opcode = opcode.value,
                .rrq = rrq.value,
            },
    };
  }
  case TFTP_OPCODE_WRQ: {
    expected_tftp_wrq_t wrq = tftp_read_wrq(&reader);
    if (!wrq.has_value)
      return (expected_tftp_packet_t){
          .has_value = false,
          .error = wrq.error,
      };

    return (expected_tftp_packet_t){
        .has_value = true,
        .value =
            {
                .opcode = opcode.value,
                .wrq = wrq.value,
            },
    };
  }
  case TFTP_OPCODE_DATA: {
    expected_tftp_data_t data = tftp_read_data(&reader);
    if (!data.has_value)
      return (expected_tftp_packet_t){
          .has_value = false,
          .error = data.error,
      };

    return (expected_tftp_packet_t){
        .has_value = true,
        .value =
            {
                .opcode = opcode.value,
                .data = data.value,
            },
    };
  }
  case TFTP_OPCODE_ACK: {
    expected_tftp_ack_t ack = tftp_read_ack(&reader);
    if (!ack.has_value)
      return (expected_tftp_packet_t){
          .has_value = false,
          .error = ack.error,
      };
    return (expected_tftp_packet_t){
        .has_value = true,
        .value =
            {
                .opcode = opcode.value,
                .ack = ack.value,
            },
    };
  }
  case TFTP_OPCODE_ERROR: {
    expected_tftp_error_t error = tftp_read_error(&reader);
    if (!error.has_value)
      return (expected_tftp_packet_t){
          .has_value = false,
          .error = error.error,
      };

    return (expected_tftp_packet_t){
        .has_value = true,
        .value =
            {
                .opcode = opcode.value,
                .error = error.value,
            },
    };
  }
  default:
    __builtin_unreachable();
  }
}

static expected_size_t tftp_rrq(tftp_buffer_t *buffer, const char *in_filename,
                                const char *in_mode) {
  tftp_buffer_view_t writer = tftp_buffer_writer(buffer);

  const expected_size_t opcode =
      tftp_buffer_write_uint16_t(&writer, TFTP_OPCODE_RRQ);
  if (!opcode.has_value)
    return (expected_size_t){
        .has_value = false,
        .error = opcode.error,
    };

  const expected_size_t filename =
      tftp_buffer_write_string(&writer, in_filename);
  if (!filename.has_value)
    return (expected_size_t){
        .has_value = false,
        .error = filename.error,
    };

  const expected_size_t mode = tftp_buffer_write_string(&writer, in_mode);
  if (!mode.has_value)
    return (expected_size_t){
        .has_value = false,
        .error = mode.error,
    };

  return (expected_size_t){
      .has_value = true,
      .value = opcode.value + filename.value + mode.value,
  };
}

static expected_size_t tftp_wrq(tftp_buffer_t *buffer, const char *in_filename,
                                const char *in_mode) {
  tftp_buffer_view_t writer = tftp_buffer_writer(buffer);

  const expected_size_t opcode =
      tftp_buffer_write_uint16_t(&writer, TFTP_OPCODE_WRQ);
  if (!opcode.has_value)
    return (expected_size_t){
        .has_value = false,
        .error = opcode.error,
    };

  const expected_size_t filename =
      tftp_buffer_write_string(&writer, in_filename);
  if (!filename.has_value)
    return (expected_size_t){
        .has_value = false,
        .error = filename.error,
    };

  const expected_size_t mode = tftp_buffer_write_string(&writer, in_mode);
  if (!mode.has_value)
    return (expected_size_t){
        .has_value = false,
        .error = mode.error,
    };

  return (expected_size_t){
      .has_value = true,
      .value = opcode.value + filename.value + mode.value,
  };
}

static expected_size_t tftp_data(tftp_buffer_t *buffer, tftp_block_t in_block,
                                 const uint8_t *in_data, size_t in_data_size) {
  tftp_buffer_view_t writer = tftp_buffer_writer(buffer);

  const expected_size_t opcode =
      tftp_buffer_write_uint16_t(&writer, TFTP_OPCODE_DATA);
  if (!opcode.has_value)
    return (expected_size_t){
        .has_value = false,
        .error = opcode.error,
    };

  const expected_size_t block = tftp_buffer_write_uint16_t(&writer, in_block);
  if (!block.has_value)
    return (expected_size_t){
        .has_value = false,
        .error = block.error,
    };

  const expected_size_t data =
      tftp_buffer_write_data(&writer, in_data, in_data_size);
  if (!data.has_value)
    return (expected_size_t){
        .has_value = false,
        .error = data.error,
    };

  return (expected_size_t){
      .has_value = true,
      .value = opcode.value + block.value + data.value,
  };
}

static expected_size_t tftp_ack(tftp_buffer_t *buffer, tftp_block_t in_block) {
  tftp_buffer_view_t writer = tftp_buffer_writer(buffer);
  expected_size_t opcode = tftp_buffer_write_uint16_t(&writer, TFTP_OPCODE_ACK);
  if (!opcode.has_value)
    return (expected_size_t){
        .has_value = false,
        .error = opcode.error,
    };

  expected_size_t block = tftp_buffer_write_uint16_t(&writer, in_block);
  if (!block.has_value)
    return (expected_size_t){
        .has_value = false,
        .error = block.error,
    };

  return (expected_size_t){
      .has_value = true,
      .value = opcode.value + block.value,
  };
}

static expected_size_t tftp_error(tftp_buffer_t *buffer, tftp_error_code_t code,
                                  const char *message) {
  tftp_buffer_view_t writer = tftp_buffer_writer(buffer);

  const expected_size_t opcode =
      tftp_buffer_write_uint16_t(&writer, TFTP_OPCODE_ERROR);
  if (!opcode.has_value)
    return (expected_size_t){
        .has_value = false,
        .error = opcode.error,
    };

  const expected_size_t error = tftp_buffer_write_uint16_t(&writer, code);
  if (!error.has_value)
    return (expected_size_t){
        .has_value = false,
        .error = error.error,
    };

  const expected_size_t msg = tftp_buffer_write_string(&writer, message);
  if (!msg.has_value)
    return (expected_size_t){
        .has_value = false,
        .error = msg.error,
    };

  return (expected_size_t){
      .has_value = true,
      .value = opcode.value + error.value + msg.value,
  };
}

static expected_tftp_packet_t tftp_recv(int socket, tftp_buffer_t *buffer,
                                        struct timeval *timeout) {
  fd_set readfds = {0};
  FD_ZERO(&readfds);
  FD_SET(socket, &readfds);

  const int ready = select(socket + 1, &readfds, NULL, NULL, timeout);
  assert(ready != -1);

  if (ready == 0)
    return (expected_tftp_packet_t){
        .has_value = false,
        .error = ETIMEDOUT,
    };

  const ssize_t bytes =
      recv(socket, &buffer->buffer, sizeof(buffer->buffer), 0);
  assert(bytes != -1);

  return tftp_parse(buffer, bytes);
}

void tftp_send_data(int socket, tftp_buffer_t *buffer, tftp_block_t block,
                    const uint8_t *in_data, size_t in_data_size) {
  const expected_size_t data = tftp_data(buffer, block, in_data, in_data_size);
  assert(data.has_value);
  assert(send(socket, &buffer->buffer, data.value, 0) != -1);
}

void tftp_send_ack(int socket, tftp_buffer_t *buffer, tftp_block_t block) {
  const expected_size_t ack = tftp_ack(buffer, block);
  assert(ack.has_value);
  assert(send(socket, &buffer->buffer, ack.value, 0) != -1);
}

void tftp_send_error(int socket, tftp_buffer_t *buffer, tftp_error_code_t code,
                     const char *message) {
  const expected_size_t error = tftp_error(buffer, code, message);
  assert(error.has_value);
  assert(send(socket, &buffer->buffer, error.value, 0) != -1);
}

void tftp_send_wrq(int socket, const char *filename, FILE *file) {
  tftp_buffer_t buffer = {0};

  const expected_size_t wrq = tftp_wrq(&buffer, filename, "netascii");
  assert(wrq.has_value);
  assert(send(socket, &buffer.buffer, wrq.value, 0) == wrq.value);

  struct timeval timeout = {
      .tv_sec = TFTP_TIMEOUT,
      .tv_usec = 0,
  };
  expected_tftp_packet_t packet = tftp_recv(socket, &buffer, &timeout);
  assert(packet.has_value);
  assert(packet.value.opcode == TFTP_OPCODE_ACK);
  assert(packet.value.ack.block == 0);

  for (;;) {
    uint8_t file_data[TFTP_BLOCK_SIZE] = {0};
    const size_t file_bytes = fread(file_data, 1, sizeof(file_data), file);

    tftp_send_data(socket, &buffer, packet.value.ack.block + 1, file_data,
                   file_bytes);
    packet = tftp_recv(socket, &buffer, NULL);
    assert(packet.has_value);
    assert(packet.value.opcode == TFTP_OPCODE_ACK);

    if (file_bytes < TFTP_BLOCK_SIZE)
      break;
  }
}

void tftp_handle_wrq(int socket, tftp_buffer_t *buffer, size_t buffer_size) {
  assert(socket != -1);
  assert(buffer);

  expected_tftp_packet_t packet = tftp_parse(buffer, buffer_size);
  assert(packet.has_value);
  assert(packet.value.opcode == TFTP_OPCODE_WRQ);

  FILE *file = fopen(packet.value.wrq.filename, "wb");
  if (!file) {
    tftp_send_error(socket, buffer, TFTP_ERROR_DISK_FULL, strerror(errno));
    return;
  }

  tftp_block_t block = 0;
  tftp_send_ack(socket, buffer, block);

  for (;;) {
    struct timeval timeout = {
        .tv_sec = TFTP_TIMEOUT,
        .tv_usec = 0,
    };
    const expected_tftp_packet_t packet = tftp_recv(socket, buffer, &timeout);

    /* handle timeout */
    if (!packet.has_value) {
      assert(packet.error == ETIMEDOUT);
      tftp_send_ack(socket, buffer, block);
      continue;
    }

    assert(packet.value.opcode == TFTP_OPCODE_DATA);
    block = packet.value.data.block;

    assert(fwrite(packet.value.data.data, 1, packet.value.data.size, file) ==
           packet.value.data.size);

    tftp_send_ack(socket, buffer, block);

    if (packet.value.data.size < TFTP_BLOCK_SIZE)
      break;
  }
}
