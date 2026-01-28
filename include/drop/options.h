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

void options_from_config(const char *filename,
                         void (*parse)(int, char *const *, options_t *),
                         options_t *out);
