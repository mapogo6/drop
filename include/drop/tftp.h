#pragma once

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>

#define TFTP_BLOCK_SIZE 512

typedef uint16_t tftp_block_t;

typedef struct {
  uint8_t buffer[TFTP_BLOCK_SIZE + 4];
} tftp_buffer_t;

typedef void (*tftp_block_cb_t)(tftp_block_t block, void *useredata);

void tftp_send_wrq(int socket, const char *filename, FILE *file,
                   tftp_block_cb_t on_block, void *userdata);
void tftp_handle_wrq(int socket, tftp_buffer_t *buffer, size_t buffer_size,
                     tftp_block_cb_t on_block, void *userdata);
