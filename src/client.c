#include <drop/options.h>
#include <drop/tftp.h>

#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef int socket_t;
typedef struct sockaddr_in6 address_t;

typedef struct {
  pid_t pid;
  const char *filename;
  int pipefd;
  struct {
    uint16_t block;
    uint16_t block_count;
  } status;
} child_t;

typedef struct {
  options_t base;
  char *const *filenames;
  size_t file_count;
} client_options_t;

#define PROGRAM_NAME "drop"

static void swap(void *a, void *b, size_t size) {
  uint8_t buffer[size];
  memcpy(buffer, a, size);
  memcpy(a, b, size);
  memcpy(b, buffer, size);
}

//clang-format off
const char *usage =
    "Usage: " PROGRAM_NAME " [options] <host> <filename> [filename...]\n"
    "\n"
    "Options:\n"
    "  --port,    -p <port> the port <host> is listening on\n"
    "  --verbose, -v        verbose output\n"
    "  --help,    -h        print this message\n"
    "\n"
    "Arguments:\n"
    "  <host>      hostname of server\n"
    "  <filename>  file to upload, - for stdin\n";
// clang-fomat on

static void options_from_argv(int argc, char *const *argv, options_t *out) {
  struct option const long_options[] = {
      {
          .name = "port",
          .has_arg = required_argument,
          .flag = NULL,
          .val = 'p',
      },
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
    switch (getopt_long(argc, argv, "p:vh", long_options, NULL)) {
    case -1:
      return;
    case 'p':
      strncpy(out->address.port, optarg, sizeof(out->address.port));
      break;
    case 'v':
      out->verbose = true;
      break;
    case 'h':
      puts(usage);
      exit(EXIT_SUCCESS);
    }
  }
}

static void parent_spawn_children(child_t *children,
                                  const client_options_t *options);
static void parent_monitor_children(child_t *children, size_t count);

int main(int argc, char *const *argv) {
  client_options_t options = {0};

  /* parse common options first, command-line can override config*/
  options_from_config("drop/" PROGRAM_NAME ".conf", &options_from_argv,
                      (options_t *)&options);
  options_from_argv(argc, argv, (options_t *)&options);

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

  options.filenames = argv + optind;
  options.file_count = argc - optind;

  child_t children[options.file_count];
  memset(children, 0, sizeof(child_t) * options.file_count);

  parent_spawn_children(children, &options);
  parent_monitor_children(children, options.file_count);

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

static socket_t child_connect(const options_t *options) {
  const socket_t s = socket(AF_INET6, SOCK_DGRAM, 0);
  if (s == -1) {
    fprintf(stderr, "socket: %s\n", strerror(errno));
    return -1;
  }

  if (-1 == setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &options->v6only,
                       sizeof(options->v6only))) {
    fprintf(stderr, "setsockopt: %s\n", strerror(errno));
    return -1;
  }

  const address_t destination = address(options);
  if (-1 ==
      connect(s, (const struct sockaddr *)&destination, sizeof(address_t))) {
    fprintf(stderr, "connect: %s\n", strerror(errno));
    return -1;
  }

  return s;
}

static void upload_file(socket_t s, const char *filename) {}

noreturn static void child(const options_t *options, child_t *child) {
  const socket_t client = child_connect(options);
  assert(client != -1);

  FILE *file = stdin;
  if (strcmp(child->filename, "-") != 0) {
    file = fopen(child->filename, "rb");
    if (!file) {
      fprintf(stderr, "fopen: %s\n", strerror(errno));
      exit(EXIT_FAILURE);
    }
  } else {
    child->filename = "stdin";
  }

  tftp_send_wrq(client, child->filename, file, NULL, NULL);

  close(client);
  close(child->pipefd);

  exit(EXIT_SUCCESS);
}

static void parent_spawn_children(child_t *children,
                                  const client_options_t *options) {
  for (size_t n = 0; n < options->file_count; ++n) {
    int fds[2] = {0};
    assert(pipe(fds) == 0);

    children[n].pid = fork();
    assert(children[n].pid != -1);

    children[n].filename = options->filenames[n];

    if (0 == children[n].pid) { /* we're the child */
      children[n].pipefd = fds[1];
      close(fds[0]);
      child(&options->base, children + n);
    }

    children[n].pipefd = fds[0];
    close(fds[1]);
    printf("spawned %d for %s\n", children[n].pid, children[n].filename);
  }
}

static void parent_cleanup_child(const child_t *child) {
  close(child->pipefd);

  int wstatus = 0;
  waitpid(child->pid, &wstatus, 0);
  if (WIFEXITED(wstatus)) {
    const int exit_code = WEXITSTATUS(wstatus);
    if (exit_code == EXIT_SUCCESS)
      printf("[%d] transer complete: %s\n", child->pid, child->filename);
    else
      fprintf(stderr, "[%d] transfer failed: %s, error: %d", child->pid,
              child->filename, exit_code);
  } else {
    fprintf(stderr, "%d finished abnormally\n", child->pid);
  }
}

static void parent_monitor_children(child_t *children, size_t count) {
  while (count) {
    fd_set readfds = {0};
    int max = 0;
    for (size_t n = 0; n < count; ++n) {
      max = (max < children[n].pipefd) ? children[n].pipefd : max;
      FD_SET(children[n].pipefd, &readfds);
    }

    if (-1 == select(max + 1, &readfds, NULL, NULL, NULL)) {
      assert(errno == EINTR);
      continue;
    }

    for (size_t n = 0; n < count; ++n) {
      if (FD_ISSET(children[n].pipefd, &readfds)) {
        const int read_result = read(children[n].pipefd, &children[n].status,
                                     sizeof(children[n].status));
        if (read_result == -1) {
          assert(errno == EINTR);
          break;
        }

        if (read_result == 0) { /* child closed pipe */
          const size_t back = --count;
          swap(children + n, children + back, sizeof(child_t));
          parent_cleanup_child(children + back);
        } else {
          assert(read_result == sizeof(children[n].status));
          printf("[%d] uploaded %d blocks of %d in %s\n", children[n].pid,
                 children[n].status.block, children[n].status.block_count,
                 children[n].filename);
        }
      }
    }
  }
}
