#pragma once

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#define TFTP_BLOCK_SIZE 512

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
} tftp_read_request_t;

typedef struct {
  const char *filename;
  const char *mode;
} tftp_write_request_t;

typedef uint16_t tftp_block_t;

typedef struct {
  tftp_block_t block;
  const uint8_t *data;
  size_t size;
} tftp_data_t;

typedef struct {
  tftp_block_t block;
} tftp_ack_t;

typedef enum tftp_err_code : uint16_t {
  TFTP_ERR_NOT_DEFINED = 0,
  TFTP_ERR_NOT_FOUND = 1,
  TFTP_ERR_ACCESS_VIOLATION = 2,
  TFTP_ERR_DISK_FULL = 3,
  TFTP_ERR_ILLEGAL_OPERATION = 4,
  TFTP_ERR_UNKNOWN_TID = 5,
  TFTP_ERR_ALREADY_EXISTS = 6,
  TFTP_ERR_NO_SUCH_USER = 7
} tftp_error_code_t;

typedef struct {
  tftp_error_code_t code;
  const char *message;
} tftp_error_t;

typedef struct {
  tftp_opcode_t opcode;
  union {
    tftp_read_request_t rrq;
    tftp_write_request_t wrq;
    tftp_data_t data;
    tftp_ack_t ack;
    tftp_error_t error;
  };
} tftp_packet_t;

typedef struct {
  uint8_t buffer[TFTP_BLOCK_SIZE + 4];
} tftp_buffer_t;

bool tftp_parse(tftp_buffer_t *buffer, size_t size, tftp_packet_t *out);

ssize_t tftp_new_rrq(tftp_buffer_t *buffer, const char *filename,
                     const char *mode);
ssize_t tftp_new_wrq(tftp_buffer_t *buffer, const char *filename,
                     const char *mode);
ssize_t tftp_new_data(tftp_buffer_t *buffer, tftp_block_t block,
                      const uint8_t *data, size_t data_size);
ssize_t tftp_new_ack(tftp_buffer_t *buffer, tftp_block_t block);
ssize_t tftp_new_error(tftp_buffer_t *buffer, tftp_error_code_t error,
                       const char *message);
