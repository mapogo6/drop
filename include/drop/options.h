#pragma once

#include <netdb.h>

typedef struct {
  char host[NI_MAXHOST];
  char port[NI_MAXSERV];
} sockname_t;

typedef struct {
  const char *id;
  sockname_t address;
  int v6only;
  int verbose;
} options_t;

#define COMMON_GETOPT_STRING "p:v"

void options_from_arguments(options_t *a, int argc, char *const *argv);
void options_from_config(options_t *a, const char *filename);
