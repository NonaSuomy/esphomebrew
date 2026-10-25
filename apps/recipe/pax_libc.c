#include "../lvgl/pax_libc.c"

#include <stddef.h>

char *strchr(const char *s, int c)
{
  if (s == NULL) return NULL;
  const char needle = (char)c;
  for (;;) {
    if (*s == needle) return (char *)s;
    if (*s == '\0') return NULL;
    ++s;
  }
}

char *strrchr(const char *s, int c)
{
  if (s == NULL) return NULL;
  const char needle = (char)c;
  const char *last = NULL;
  for (;;) {
    if (*s == needle) last = s;
    if (*s == '\0') return (char *)last;
    ++s;
  }
}
