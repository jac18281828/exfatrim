#include <stdlib.h>
#include <string.h>

#include "shell.h"

static int is_bare(const char* s) {
  if (!*s)
    return 0;
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    int plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || strchr("_./:=@%+,-", c);
    if (!plain)
      return 0;
  }
  return 1;
}

static size_t quoted_length(const char* s) {
  if (is_bare(s))
    return strlen(s);
  size_t n = 2;
  for (; *s; s++)
    n += *s == '\'' ? 4 : 1;
  return n;
}

static char* put_quoted(char* out, const char* s) {
  if (is_bare(s)) {
    size_t n = strlen(s);
    memcpy(out, s, n);
    return out + n;
  }
  *out++ = '\'';
  for (; *s; s++) {
    if (*s == '\'') {
      memcpy(out, "'\\''", 4);
      out += 4;
    } else {
      *out++ = *s;
    }
  }
  *out++ = '\'';
  return out;
}

char* shell_join(int argc, char* const* argv) {
  size_t total = 1;
  for (int i = 0; i < argc; i++)
    total += quoted_length(argv[i]) + 1;
  char* line = malloc(total);
  if (!line)
    return NULL;
  char* out = line;
  for (int i = 0; i < argc; i++) {
    if (i)
      *out++ = ' ';
    out = put_quoted(out, argv[i]);
  }
  *out = '\0';
  return line;
}
