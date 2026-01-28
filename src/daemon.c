#include <drop/options.h>
#include <drop/tftp.h>

#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
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
  "Usage: " PROGRAM_NAME " [options] [bind[:port]]\n"
  "\n"
  "Arguemnts:\n"
  "  bind: the address to to listen on. Default is the 'any' address\n"
  "  port: the port to listen on. Default is '0'\n"
  "Options:\n"
  "  -v, --verbose verbose output\n"
  "  -h, --help    print this message\n";
// clang-format on

/* creates address_t from options */
static address_t address(const options_t *options) {
  struct addrinfo hints = {0};
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = 0;
  hints.ai_family = AF_INET6;
  hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV | AI_ADDRCONFIG;

  const char *host =
      strlen(options->address.host) == 0 ? NULL : options->address.host;
  const char *port = options->address.port;

  struct addrinfo *results = NULL;
  const int ret = getaddrinfo(host, port, &hints, &results);

  switch (ret) {
  case 0:
    break;
  case EAI_AGAIN:
    return address(options);
  case EAI_SYSTEM:
    /*error*/ fprintf(stderr, "getaddrinfo failed: '%s'\n", strerror(errno));
    goto err;
  default:
    /*error*/ fprintf(stderr, "getaddrinfo failed: %s\n", gai_strerror(ret));
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

static int udp_accept(socket_t listen_socket, uint16_t listen_port,
                      void *buffer, size_t *buffer_size);
static int loop(socket_t socket, const address_t *bind_address);
static void options_from_argv(int argc, char *const *argv, options_t *out);

int main(int argc, char **argv) {
  server_options_t options = {0};

  /* disable getopt printing error */
  opterr = 0;

  /* parse common options first, command-line can override config */
  options_from_config(PROGRAM_NAME ".conf", &options_from_argv,
                      (options_t *)&options);
  options_from_argv(argc, argv, (options_t *)&options);

  socket_t s = socket(AF_INET6, SOCK_DGRAM, 0);
  if (s == -1) {
    /*error*/ fprintf(stderr, "socket() failed: '%s'\n", strerror(errno));
    exit(EXIT_FAILURE);
  }

  int reuseaddr = 1;
  if (-1 ==
      setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof(reuseaddr))) {
    /*error*/ fprintf(stderr, "setsockopt for 'SO_REUSEADDR': %s\n",
                      strerror(errno));
    exit(EXIT_FAILURE);
  }

  if (-1 == setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &options.base.v6only,
                       sizeof(options.base.v6only))) {
    /*error*/ fprintf(stderr, "setsockopt for 'IPV6_V6ONLY': '%s'\n",
                      strerror(errno));
    exit(EXIT_FAILURE);
  }

  int pktinfo = 1;
  if (-1 == setsockopt(s, IPPROTO_IPV6, IPV6_RECVPKTINFO, &pktinfo,
                       sizeof(pktinfo))) {
    /*error*/ fprintf(stderr, "setsockopt for 'IPV6_RECVPKTINFO': %s\n",
                      strerror(errno));
    exit(EXIT_FAILURE);
  }

  address_t server = address(&options.base);
  if (-1 == bind(s, (struct sockaddr *)&server, sizeof(server))) {
    /*error*/ fprintf(stderr, "bind failed: '%s'\n", strerror(errno));
    exit(EXIT_FAILURE);
  }

  socklen_t serverlen = sizeof(server);
  if (-1 == getsockname(s, (struct sockaddr *)&server, &serverlen)) {
    /*error*/ fprintf(stderr, "getsockname: %s\n", strerror(errno));
  }

  return loop(s, &server);
}

static ssize_t recvmessage(socket_t s, void *buffer, size_t buffer_size,
                           int flags, address_t *src, address_t *dst) {
  assert(buffer);

  struct iovec iov = {
      .iov_base = buffer,
      .iov_len = buffer_size,
  };

  uint8_t cmsg_storage[CMSG_SPACE(sizeof(struct in6_pktinfo))] = {0};

  struct msghdr msg = {0};
  msg.msg_name = src;
  msg.msg_namelen = src ? sizeof(address_t) : 0;
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = cmsg_storage;
  msg.msg_controllen = sizeof(cmsg_storage);

  const ssize_t received = recvmsg(s, &msg, flags);

  if (-1 != received) {
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
      if (cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_PKTINFO) {
        struct in6_pktinfo *pktinfo = (struct in6_pktinfo *)CMSG_DATA(cmsg);
        memset(dst, 0, sizeof(address_t));
        dst->sin6_family = AF_INET6;
        dst->sin6_addr = pktinfo->ipi6_addr;
        if (IN6_IS_ADDR_LINKLOCAL(&pktinfo->ipi6_addr) ||
            IN6_IS_ADDR_MC_LINKLOCAL(&pktinfo->ipi6_addr)) {
          dst->sin6_scope_id = pktinfo->ipi6_ifindex;
        }
      }
    }
  }

  return received;
}

static int udp_accept(socket_t listen_socket, uint16_t listen_port,
                      void *buffer, size_t *buffer_size) {
  assert(listen_socket != -1);
  assert(listen_port != 0);
  assert(buffer);
  assert(buffer_size);

  address_t source = {0};
  address_t destination = {0};

  const ssize_t bytes = recvmessage(listen_socket, buffer, *buffer_size, 0,
                                    &source, &destination);
  if (bytes == -1) {
    /*error*/ fprintf(stderr, "recvmessage: %s\n", strerror(errno));
    return -1;
  }

  destination.sin6_port = htons(listen_port);

  const socket_t s = socket(AF_INET6, SOCK_DGRAM, 0);
  if (s == -1) {
    /*error*/ fprintf(stderr, "socket: %s\n", strerror(errno));
    return -1;
  }

  int reuseaddr = 1;
  if (-1 ==
      setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof(reuseaddr))) {
    /*error*/ fprintf(stderr, "setsockopt for 'SO_REUSEADDR': %s\n",
                      strerror(errno));
    goto err;
  }

  if (-1 == bind(s, (struct sockaddr *)&destination, sizeof(address_t))) {
    /*error*/ fprintf(stderr, "bind: %s\n", strerror(errno));
    goto err;
  }

  if (-1 == connect(s, (struct sockaddr *)&source, sizeof(address_t))) {
    /*error*/ fprintf(stderr, "connect: %s\n", strerror(errno));
    goto err;
  }

  *buffer_size = (size_t)bytes;
  return s;

err:
  close(s);
  return -1;
}

int loop(socket_t socket, const address_t *bind_address) {
  sockname_t servername = {0};
  assert(sockname(bind_address, &servername));

  /*verbose*/
  printf("listening on %s:%s\n", servername.host, servername.port);

  for (;;) {
    tftp_buffer_t buffer = {0};

    size_t received = sizeof(buffer.buffer);
    const socket_t client = udp_accept(socket, ntohs(bind_address->sin6_port),
                                       &buffer.buffer, &received);

    if (client == -1) {
      continue;
    }

    const pid_t pid = fork();
    switch (pid) {
    case -1:
      fprintf(stderr, "fork: %s\n", strerror(errno));
      close(client);
      break;
    case 0: /* we're the child */
      close(socket);
      tftp_handle_wrq(client, &buffer, received, NULL, NULL);
      close(client);
      return 0;
    default: /* we're the parent */
      close(client);
      break;
    }
  }
}

static void options_from_argv(int argc, char *const *argv, options_t *out) {
  struct option const long_options[] = {
      {
          .name = "verbose",
          .has_arg = no_argument,
          .flag = NULL,
          .val = 'v',
      },
      {
          .name = "help",
          .has_arg = no_argument,
          .flag = NULL,
          .val = 'h',
      },
      {.name = 0, .has_arg = 0, .flag = 0, .val = 0},
  };

  optind = 1;

  for (;;) {
    switch (getopt_long(argc, argv, "vh", long_options, NULL)) {
    case -1:
      return;
    case 'v':
      out->verbose = true;
      break;
    case 'h':
      puts(usage);
      exit(EXIT_SUCCESS);
    default:
      exit(EXIT_FAILURE);
    }
  }
}
