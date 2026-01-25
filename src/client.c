#include <drop/options.h>
#include <drop/tftp.h>

#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef int socket_t;
typedef struct sockaddr_in6 address_t;

typedef struct {
  options_t base;
} client_options_t;

#define PROGRAM_NAME "drop"

//clang-format off
const char *usage =
    "Usage: " PROGRAM_NAME " [options] <host> <filename> [filename...]\n"
    "\n"
    "Options:\n"
    "  -p  <port>      the port <host> is listening on\n"
    "  -v              verbose output\n"
    "  -h              print this message\n"
    "\n"
    "Arguments:\n"
    "  <host>      hostname of server\n"
    "  <filename>  file to upload, - for stdin\n";
// clang-fomat on

static void upload_file(const options_t *options, const char *filename);

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
    fprintf(stderr, PROGRAM_NAME ": expected <filename> argument\n");
    puts(usage);
    exit(EXIT_FAILURE);
  }

  /* read positional arguments */
  strncpy(options.base.address.host, argv[optind++],
          sizeof(options.base.address.host));

  for (; optind < argc; ++optind) {
    const pid_t pid = fork();
    switch (pid) {
    case -1:
      fprintf(stderr, "fork: %s\n", strerror(errno));
      exit(EXIT_FAILURE);
    case 0: /* we're the child */
      upload_file(&options.base, argv[optind]);
      return EXIT_SUCCESS;
    default: /* we're the parent */
      continue;
    }
  }

  int wstatus = 0;
  for (;;) {
    const pid_t child = waitpid(-1, &wstatus, 0);
    if (child == -1) {
      if (errno == EINTR) /* interrupted */
        continue;
      if (errno == ECHILD) /* all children exited */
        break;
    } else if (WIFEXITED(wstatus)) {
      printf("[%d] exited normally: %d\n", child, WEXITSTATUS(wstatus));
    } else if (WIFSIGNALED(wstatus)) {
      fprintf(stderr, "[%d] killed by signal: %d\n", child, WTERMSIG(wstatus));
    } else {
      fprintf(stderr, "[%d] exitd abnormally\n", child);
    }
  }

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

static void upload_file(const options_t *options, const char *filename) {
  const socket_t s = socket(AF_INET6, SOCK_DGRAM, 0);
  if (s == -1) {
    fprintf(stderr, "socket: %s\n", strerror(errno));
    exit(EXIT_FAILURE);
  }

  if (-1 == setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &options->v6only,
                       sizeof(options->v6only))) {
    fprintf(stderr, "setsockopt: %s\n", strerror(errno));
    exit(EXIT_FAILURE);
  }

  const address_t destination = address(options);
  if (-1 ==
      connect(s, (const struct sockaddr *)&destination, sizeof(address_t))) {
    fprintf(stderr, "connect: %s\n", strerror(errno));
    exit(EXIT_FAILURE);
  }

  FILE *file = stdin;
  if (strcmp(filename, "-") != 0) {
    file = fopen(filename, "rb");
    if (!file) {
      fprintf(stderr, "fopen: %s\n", strerror(errno));
      exit(EXIT_FAILURE);
    }
  } else {
    filename = "stdin";
  }

  tftp_send_wrq(s, filename, file);
}
