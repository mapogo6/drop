#include <drop/options.h>
#include <drop/tftp.h>

#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

typedef int socket_t;
typedef struct sockaddr_in6 address_t;

typedef struct {
  options_t base;
  const char *filename;
} client_options_t;

#define PROGRAM_NAME "drop"

//clang-format off
const char *usage =
    "Usage: " PROGRAM_NAME " [options] <host> <filename>\n"
    "\n"
    "Options:\n"
    "  -i  <interface> the interace on which to discover servers\n"
    "  -p  <port>      the port <host> is listening on\n"
    "  -v              verbose output\n"
    "  -h              print this message\n"
    "\n"
    "Arguments:\n"
    "  <host>      hostname of server\n"
    "  <filename>  file to upload, - for stdin\n";
// clang-fomat on

static address_t address(const options_t *options);

static void tftp_upload_file(socket_t s, address_t server, char *filename);

int main(int argc, char **argv) {
  client_options_t options;

  /* disable getopt printing errors */
  opterr = 0;

  /* parse common options first, command-line can override config*/
  options_from_config((options_t *)&options, "drop.conf");
  options_from_arguments((options_t *)&options, argc, argv);

  /* reset getopt for common options */
  optind = 1;

  /* read app specific options */
  int option = 0;
  while ((option = getopt(argc, argv, COMMON_GETOPT_STRING "h")) != -1) {
    switch (option) {
    case 'h':
      puts(usage);
      exit(EXIT_SUCCESS);
    default: /* ignore unknown options */
      break;
    }
  }

  if (optind == argc) {
    fprintf(stderr,
            PROGRAM_NAME ": expected <host> and <filename> arguments\n");
    puts(usage);
    exit(EXIT_FAILURE);
  }

  if (optind + 1 == argc) {
    fprintf(stderr, PROGRAM_NAME ": expected <filename argument\n");
    puts(usage);
    exit(EXIT_FAILURE);
  }

  /* read positional arguments */
  strncpy(options.base.address.host, argv[optind++],
          sizeof(options.base.address.host));
  options.filename = argv[optind++];

  const socket_t s = socket(AF_INET6, SOCK_DGRAM, 0);
  if (s == -1) {
    fprintf(stderr, "socket: %s\n", strerror(errno));
    exit(EXIT_FAILURE);
  }

  if (-1 == setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &options.base.v6only,
                       sizeof(options.base.v6only))) {
    fprintf(stderr, "setsockopt: %s\n", strerror(errno));
    exit(EXIT_FAILURE);
  }

  const address_t server = address((options_t *)&options);
  tftp_upload_file(s, server, strdup(options.filename));

  return EXIT_SUCCESS;
}

address_t address(const options_t *options) {
  struct addrinfo hints = {0};
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = 0;
  hints.ai_family = AF_INET6;
  hints.ai_flags = AI_NUMERICSERV | AI_ADDRCONFIG;

  if (!options->v6only) {
    hints.ai_flags |= AI_V4MAPPED | AI_ALL;
  }

  const char *host =
      strlen(options->address.host) == 0 ? NULL : options->address.host;
  const char *port = options->address.port;

  struct addrinfo *results = NULL;
  int ret = getaddrinfo(host, port, &hints, &results);

  switch (ret) {
  case 0:
    break;
  case EAI_AGAIN:
    return address(options);
  case EAI_SYSTEM:
    fprintf(stderr, "getaddrinfo failed: '%s'\n", strerror(errno));
    goto err;
  default:
    fprintf(stderr, "getaddrinfo failed: %s\n", gai_strerror(ret));
    goto err;
  }

  assert(results);
  assert(results->ai_family == AF_INET6);

  address_t addr = {0};
  memcpy(&addr, results->ai_addr, sizeof(addr));

  freeaddrinfo(results);
  return addr;

err:
  freeaddrinfo(results);
  exit(EXIT_FAILURE);
}

static void tftp_send_write_request(socket_t s, address_t server,
                                    tftp_buffer_t *buffer,
                                    const char *filename) {
  ssize_t request_size = tftp_new_wrq(buffer, filename, "netascii");
  if (request_size == -1) {
    fprintf(stderr, "tfpt_new_wrq: %s\n", strerror(errno));
    exit(EXIT_FAILURE);
  }

  if (-1 == sendto(s, buffer->buffer, request_size, 0,
                   (struct sockaddr *)&server, sizeof(server))) {
    fprintf(stderr, "sendto: %s\n", strerror(errno));
    exit(EXIT_FAILURE);
  }
}

static tftp_ack_t tftp_recv_ack(socket_t s, tftp_buffer_t *buffer) {
  const ssize_t received = recv(s, buffer->buffer, sizeof(buffer->buffer), 0);
  if (-1 == received) {
    fprintf(stderr, "recv: %s\n", strerror(errno));
    exit(EXIT_FAILURE);
  }

  tftp_packet_t packet = {0};
  if (!tftp_parse(buffer, received, &packet)) {
    fprintf(stderr, "tftp_parse: %s\n", strerror(errno));
    exit(EXIT_FAILURE);
  }

  assert(packet.opcode == TFTP_OPCODE_ACK);
  return packet.ack;
}

static void tftp_send_data(socket_t s, address_t server, tftp_buffer_t *buffer,
                           tftp_block_t block, const uint8_t *data,
                           size_t data_size) {
  const ssize_t packet_size =
      tftp_new_data(buffer, block, (const uint8_t *)data, data_size);
  if (-1 == packet_size) {
    fprintf(stderr, "tfpt_new_data: %s\n", strerror(errno));
    exit(EXIT_FAILURE);
  }

  if (-1 == sendto(s, buffer->buffer, packet_size, 0,
                   (struct sockaddr *)&server, sizeof(server))) {
    fprintf(stderr, "sendto: %s\n", strerror(errno));
    exit(EXIT_FAILURE);
  }
}

static void tftp_upload_file(socket_t s, address_t server, char *filename) {
  FILE *file = stdin;
  if (strcmp(filename, "-") != 0) {
    file = fopen(filename, "rb");
    if (!file) {
      fprintf(stderr, "fopen: %s\n", strerror(errno));
      exit(EXIT_FAILURE);
    }
  }

  tftp_buffer_t transfer = {0};
  tftp_send_write_request(s, server, &transfer, basename(filename));
  tftp_recv_ack(s, &transfer);

  free(filename);

  uint8_t file_data[TFTP_BLOCK_SIZE] = {0};
  size_t file_bytes = 0;
  tftp_block_t block = 1;

  while ((file_bytes = fread(file_data, 1, sizeof(file_data), file)) != 0) {
  rtx:
    tftp_send_data(s, server, &transfer, block, file_data, file_bytes);
    if (tftp_recv_ack(s, &transfer).block != block)
      goto rtx;
    ++block;
  }
}
