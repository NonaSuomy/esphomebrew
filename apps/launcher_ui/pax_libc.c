/* The launcher uses the same freestanding C runtime as the LVGL reference
 * app.  Keeping one implementation avoids subtly different memcpy/memset
 * code in the two PAXs. */
#include "../lvgl/pax_libc.c"

char *strchr(const char *s, int c) {
  do { if (*s == (char)c) return (char *)s; } while (*s++);
  return NULL;
}
char *strstr(const char *s, const char *needle) {
  size_t n = strlen(needle);
  if (!n) return (char *)s;
  for (; *s; ++s) {
    size_t i = 0;
    while (i < n && s[i] && s[i] == needle[i]) ++i;
    if (i == n) return (char *)s;
  }
  return NULL;
}
