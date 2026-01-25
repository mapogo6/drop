#pragma once

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>

#define TFTP_BLOCK_SIZE 512

typedef struct {
  uint8_t buffer[TFTP_BLOCK_SIZE + 4];
} tftp_buffer_t;

void tftp_send_wrq(int socket, const char *filename, FILE *file);
void tftp_handle_wrq(int socket, tftp_buffer_t *buffer, size_t buffer_size);
