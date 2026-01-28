#include <drop/options.h>

#include <assert.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wordexp.h>

inline static char *strf(const char *fmt, ...) {
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

static void *safe_realloc(void *ptr, size_t old_size, size_t new_size) {
  ptr = realloc(ptr, new_size);
  if (!ptr)
    fprintf(stderr, "realloc failed!\n"), abort();
  if (new_size > old_size)
    memset(ptr + old_size, 0, new_size - old_size);
  return ptr;
}

static char *str_realloc_fmt(char *ptr, const char *fmt, ...) {
  assert(fmt);

  va_list args1, args2;
  va_start(args1, fmt);
  va_copy(args2, args1);

  const size_t old_size = ptr ? strlen(ptr) + 1 : 0;
  const size_t new_size = vsnprintf(NULL, 0, fmt, args1) + 1;

  ptr = safe_realloc(ptr, old_size, new_size);

  vsnprintf(ptr, new_size, fmt, args2);

  va_end(args2);
  va_end(args1);

  return ptr;
}

static void options_from_config_file(
    FILE *file, void (*parse)(int, char *const *, options_t *), options_t *out);

#define DIR_SEPARATOR "/"

void options_from_config(const char *filename,
                         void (*parse)(int, char *const *, options_t *),
                         options_t *out) {
  const char *config_dir = getenv("XDG_CONFIG_HOME");
  assert(config_dir);

  char *path = strf("%s" DIR_SEPARATOR "%s", config_dir, filename);

  FILE *file = fopen(path, "rb");
  if (file) {
    options_from_config_file(file, parse, out);
    fclose(file);
  }

  free(path);
}

static void options_from_config_file(FILE *file,
                                     void (*parse)(int, char *const *,
                                                   options_t *),
                                     options_t *out) {
  char **argv = malloc(3 * sizeof(char *));
  argv[0] = "config";
  argv[1] = NULL;
  argv[2] = NULL;

  char *line = NULL;
  size_t capacity = 0;
  while (getline(&line, &capacity, file) != 0) {
    for (char *newline = strchr(line, '\n'); newline;
         newline = strchr(line, '\n'))
      *newline = ' ';

    wordexp_t result = {0};

    if (wordexp(line, &result, WRDE_NOCMD) == 0) {
      argv[1] = str_realloc_fmt(argv[1], "--%s", result.we_wordv[0]);
      if (result.we_wordc == 2) {
        argv[2] = result.we_wordv[1];
      }
      parse(result.we_wordc + 1, argv, out);
    }

    wordfree(&result);
  }

  if (line)
    free(line);
  if (argv[1])
    free(argv[1]);
  free(argv);
}
