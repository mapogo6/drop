#include <drop/options.h>
#include <drop/tftp.h>

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

typedef int socket_t;
typedef struct sockaddr_in6 address_t;

typedef struct {
  options_t base;
  const char *id;
} server_options_t;

#define PROGRAM_NAME "dropd"

// clang-format off
const char *usage =
  "Usage: " PROGRAM_NAME " [options]\n"
  "\n"
  "Options:\n"
  "  -p  <port>       the <port> server will listen on\n"
  "  -v               verbose output\n"
  "  -h               print this message\n";
// clang-format on

/* creates address_t from options */
static address_t address(const server_options_t *options) {
  struct addrinfo hints = {0};
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = 0;
  hints.ai_family = AF_INET6;
  hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV | AI_ADDRCONFIG;

  const char *host = strlen(options->base.address.host) == 0
                         ? NULL
                         : options->base.address.host;
  const char *port = options->base.address.port;

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

static bool sockname(const address_t *addr, sockname_t *out) {
  char host[NI_MAXHOST] = {0}, serv[NI_MAXSERV] = {0};
  if (0 != getnameinfo((struct sockaddr *)addr, sizeof(address_t), host,
                       sizeof(host), serv, sizeof(serv),
                       NI_NUMERICHOST | NI_NUMERICSERV)) {

    return false;
  }

  strcpy(out->host, host);
  strcpy(out->port, serv);
  return true;
}

static void tftp_send_ack(socket_t s, const address_t *client,
                          tftp_buffer_t *packet, tftp_block_t block) {
  const ssize_t ack_size = tftp_new_ack(packet, block);
  assert(ack_size != -1);

  if (-1 == sendto(s, packet->buffer, ack_size, 0, (struct sockaddr *)client,
                   sizeof(address_t))) {
    fprintf(stderr, "sendto failed: %s\n", strerror(errno));
    exit(EXIT_FAILURE);
  }

  /* verbose */ printf("[%d] <<< opcode: ack, block: %d\n",
                       ntohs(client->sin6_port), block);
}

static void tftp_send_error(socket_t s, const address_t *client,
                            tftp_buffer_t *packet, tftp_error_code_t code,
                            const char *message) {
  ssize_t error_size = tftp_new_error(packet, code, message);
  if (-1 == error_size) {
    fprintf(stderr, "tftp_new_error: %s\n", strerror(errno));
    exit(EXIT_FAILURE);
  }

  if (-1 == sendto(s, packet->buffer, error_size, 0, (struct sockaddr *)client,
                   sizeof(address_t))) {
    fprintf(stderr, "sendto: %s\n", strerror(errno));
    exit(EXIT_FAILURE);
  }

  /* error */ printf("[%d] opcode: err, code: %d, message: %s\n",
                     ntohs(client->sin6_port), code, message);
}

typedef struct {
  uint16_t tid;
  FILE *file;
  tftp_block_t last_block;
} tftp_session_t;

tftp_session_t *tftp_session_init(tftp_session_t *session, uint16_t tid) {
  assert(session);

  memset(session, 0, sizeof(tftp_session_t));
  session->tid = tid;

  return session;
}

void tftp_session_finalize(tftp_session_t *session) {
  assert(session);
  if (session->file)
    fclose(session->file);
}

typedef struct {
  tftp_session_t *data;
  size_t size;
  size_t capacity;
} tftp_sessions_list_t;

tftp_session_t *tftp_sessions_list_find(tftp_sessions_list_t *sessions,
                                        uint16_t tid) {
  assert(sessions);

  for (size_t i = 0; i < sessions->size; ++i)
    if (sessions->data[i].tid == tid)
      return sessions->data + i;

  return NULL;
}

tftp_session_t *tftp_sessions_list_push(tftp_sessions_list_t *sessions,
                                        uint16_t tid) {
  assert(sessions);
  assert(!tftp_sessions_list_find(sessions, tid));

  const size_t new_size = sessions->size + 1;
  if (new_size > sessions->capacity) {
    if (sessions->capacity == 0) {
      sessions->capacity = new_size;
    }
    sessions->data = realloc(sessions->data,
                             sizeof(tftp_session_t) * sessions->capacity * 2);
    assert(sessions->data);
    sessions->capacity *= 2;
  }

  return tftp_session_init(sessions->data + sessions->size++, tid);
}

void tftp_session_swap(tftp_session_t *lhs, tftp_session_t *rhs) {
  assert(lhs);
  assert(rhs);

  tftp_session_t tmp = {0};
  memcpy(&tmp, lhs, sizeof(tftp_session_t));
  memcpy(lhs, rhs, sizeof(tftp_session_t));
  memcpy(rhs, &tmp, sizeof(tftp_session_t));
}

void tftp_sessions_list_pop(tftp_sessions_list_t *sessions, uint16_t tid) {
  for (size_t i = 0; i < sessions->size; ++i)
    if (sessions->data[i].tid == tid) {
      sessions->size -= 1;
      tftp_session_swap(sessions->data + i, sessions->data + sessions->size);
      tftp_session_finalize(sessions->data + sessions->size);
    }
}

void tftp_sessions_list_clear(tftp_sessions_list_t *sessions) {
  for (size_t i = 0; i < sessions->size; ++i)
    tftp_session_finalize(sessions->data + i);

  if (sessions->data)
    free(sessions->data);

  memset(sessions, 0, sizeof(tftp_sessions_list_t));
}

typedef struct {
  bool has_value;
  union {
    tftp_block_t value;
    tftp_error_t error;
  };
} expected_tftp_block_t;

expected_tftp_block_t tftp_session_advance(tftp_sessions_list_t *sessions,
                                           const address_t *source,
                                           tftp_buffer_t *buffer, size_t size) {
  assert(sessions);
  assert(source);
  assert(buffer);

  tftp_packet_t packet;
  assert(tftp_parse(buffer, size, &packet));

  const uint16_t tid = ntohs(source->sin6_port);

  tftp_session_t *session = tftp_sessions_list_find(sessions, tid);
  if (!session) {
    sockname_t source_name = {0};
    assert(sockname(source, &source_name));

    /* info */ printf("[%d] new transaction from '%s'\n", tid,
                      source_name.host);
    session = tftp_sessions_list_push(sessions, tid);
  }

  assert(session);

  switch (packet.opcode) {
  case TFTP_OPCODE_WRQ:
    assert(!session->file);
    assert(session->last_block == 0);

    /* info */ printf("[%d] >>> opcode: wrq, filename: '%s', mode: '%s'\n", tid,
                      packet.wrq.filename, packet.wrq.mode);

    session->file = fopen(packet.wrq.filename, "wb");
    assert(session->file); /* todo: handle error here */

    return (expected_tftp_block_t){
        .has_value = true,
        .value = 0,
    };
  case TFTP_OPCODE_DATA: {
    assert(session->file);

    /* verbose */ printf("[%d] >>> opcode: data, block: %d, size: %zd\n", tid,
                         packet.data.block, packet.data.size);

    /* check if this is a retransmit */
    if (packet.data.block == session->last_block) {
      return (expected_tftp_block_t){
          .has_value = true,
          .value = packet.data.block,
      };
    }

    /* todo: handle an out-of-order block correctly */
    assert(session->last_block + 1 == packet.data.block);
    session->last_block = packet.data.block;

    const size_t written =
        fwrite(packet.data.data, 1, packet.data.size, session->file);
    if (written != packet.data.size) {
      tftp_sessions_list_pop(sessions, tid);
      return (expected_tftp_block_t){
          .has_value = false,
          .error = TFTP_ERR_DISK_FULL,
      };
    }

    /* if this is the last chunk, stop tracking this session */
    /* todo: how to handle rtx of last block? */
    if (written != TFTP_BLOCK_SIZE) {
      /* info */ printf("[%d] transaction complete\n", tid);
      tftp_sessions_list_pop(sessions, tid);
    }
    return (expected_tftp_block_t){
        .has_value = true,
        .value = packet.data.block,
    };
  }
  default:
    __builtin_unreachable();
  }
}

static void tftp_transfer(socket_t s, tftp_sessions_list_t *sessions) {
  assert(s != -1);
  assert(sessions);

  address_t source = {0};
  socklen_t source_len = sizeof(source);

  tftp_buffer_t buffer = {0};
  const ssize_t bytes = recvfrom(s, buffer.buffer, sizeof(buffer.buffer), 0,
                                 (struct sockaddr *)&source, &source_len);
  if (bytes == -1) {
    fprintf(stderr, "recv failed: '%s'", strerror(errno));
    exit(EXIT_FAILURE);
  }

  expected_tftp_block_t block =
      tftp_session_advance(sessions, &source, &buffer, bytes);
  assert(block.has_value);

  if (block.has_value) {
    tftp_send_ack(s, &source, &buffer, block.value);
  } else {
    tftp_send_error(s, &source, &buffer, block.error.code, block.error.message);
  }
}

int main(int argc, char **argv) {
  server_options_t options = {0};

  /* disable getopt printing error */
  opterr = 0;

  /* parse common options first, command-line can override config */
  options_from_config((options_t *)&options, "dropd.conf");
  options_from_arguments((options_t *)&options, argc, argv);

  /* reset getopt for common options */
  optind = 1;

  int option = 0;
  while ((option = getopt(argc, argv, COMMON_GETOPT_STRING "h")) != -1) {
    switch (option) {
    case 'h':
      puts(usage);
      exit(EXIT_SUCCESS);
    default: /* ignore unknown options, they may be common */
      break;
    }
  }

  socket_t s = socket(AF_INET6, SOCK_DGRAM, 0);
  if (s == -1) {
    fprintf(stderr, "socket() failed: '%s'\n", strerror(errno));
    exit(EXIT_FAILURE);
  }

  if (-1 == setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &options.base.v6only,
                       sizeof(options.base.v6only))) {
    fprintf(stderr, "setsockopt for 'IPV6_V6ONLY' option: '%s'\n",
            strerror(errno));
    exit(EXIT_FAILURE);
  }

  address_t server = address(&options);
  if (-1 == bind(s, (struct sockaddr *)&server, sizeof(server))) {
    fprintf(stderr, "bind failed: '%s'\n", strerror(errno));
    exit(EXIT_FAILURE);
  }

  socklen_t serverlen = sizeof(server);
  if (-1 == getsockname(s, (struct sockaddr *)&server, &serverlen)) {
    fprintf(stderr, "getsockname: %s\n", strerror(errno));
  }

  sockname_t servername = {0};
  assert(sockname(&server, &servername));
  printf("listening on %s:%s\n", servername.host, servername.port);

  tftp_sessions_list_t sessions = {0};
  for (;;) {
    tftp_transfer(s, &sessions);
  }
  tftp_sessions_list_clear(&sessions);

  return 0;
}
