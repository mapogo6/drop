#include <drop/options.h>

#include <getopt.h>
#include <stdbool.h>
#include <stdnoreturn.h>
#include <string.h>

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

void options_from_arguments(options_t *options, int argc, char **argv) {
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

void options_from_config(options_t *options, const char *path) {}
