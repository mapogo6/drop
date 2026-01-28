#include <drop/options.h>

#include <assert.h>
#include <ctype.h>
#include <getopt.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>

static char *strfmt(const char *fmt, ...) {
  va_list args1, args2;
  va_start(args1, fmt);
  va_copy(args2, args1);

  const int size = vsnprintf(NULL, 0, fmt, args1);
  assert(size > 0);

  char *str = malloc(size + 1);

  vsnprintf(str, size + 1, fmt, args2);

  va_end(args2);
  va_end(args1);

  return str;
}

options_t options_new(void) {
  options_t options = {
      .id = "some-random-id",
      .address =
          {
              .host = {0},
              .port = {0},
          },
      .v6only = 0,
  };
  return options;
}

void options_from_arguments(options_t *options, int argc, char *const *argv) {
  optind = 1;

  int option;

  while ((option = getopt(argc, argv, COMMON_GETOPT_STRING)) != -1) {
    switch (option) {
    case 'p':
      strncpy(options->address.port, optarg, sizeof(options->address.port));
      break;
    case 'v':
      options->verbose = true;
      break;
    default:
      break;
    }
  }
}

char **argv_from_file(const char *path, int *argc_out) {
  FILE *fp = fopen(path, "r");
  if (!fp)
    return NULL;

  size_t cap = 16;
  int argc = 1; // argv[0] is dummy program name
  char **argv = malloc(cap * sizeof(*argv));

  argv[0] = strdup("config");

  char line[1024];

  while (fgets(line, sizeof line, fp)) {
    char *p = line;

    /* Skip leading whitespace */
    while (isspace((unsigned char)*p))
      p++;

    /* Ignore blank lines and comments */
    if (*p == '\0' || *p == '#')
      continue;

    /* Tokenize */
    char *tok = strtok(p, " \t\r\n");
    if (!tok)
      continue;

    /* Ensure capacity */
    if (argc + 2 >= (int)cap) {
      cap *= 2;
      argv = realloc(argv, cap * sizeof(*argv));
    }

    /* Prefix option with - */
    argv[argc++] = strfmt("-%s", tok);

    /* Remaining tokens are arguments */
    while ((tok = strtok(NULL, " \t\r\n"))) {
      if (argc + 1 >= (int)cap) {
        cap *= 2;
        argv = realloc(argv, cap * sizeof(*argv));
      }
      argv[argc++] = strdup(tok);
    }
  }

  fclose(fp);

  argv[argc] = NULL;
  *argc_out = argc;
  return argv;
}

#define DIR_SEPARATOR "/"

void options_from_config(options_t *options, const char *filename) {
  const char *config_dir = getenv("XDG_CONFIG_HOME");
  assert(config_dir);

  char *path = strfmt("%s" DIR_SEPARATOR "%s", config_dir, filename);

  int argc = 0;
  char **argv = argv_from_file(path, &argc);
  if (argv) {
    options_from_arguments(options, argc, argv);
    free(argv);
  }

  free(path);
}
