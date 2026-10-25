/* Sanitized host smoke for the real baseline JPEG decode path. */
#include "relay_bridge.c"
#include <stdio.h>
int main(int argc, char **argv) {
  assert(argc == 2);
  FILE *f = fopen(argv[1], "rb");
  assert(f);
  assert(!fseek(f, 0, SEEK_END));
  long n = ftell(f);
  assert(n > 0);
  rewind(f);
  unsigned char *data = malloc((size_t)n);
  assert(data);
  assert(fread(data, 1, (size_t)n, f) == (size_t)n);
  fclose(f);
  test_t *t = test_relay_new(64, 48);
  test_relay_feed(t, data, (size_t)n);
  for (int i = 0; i < 2000; i++)
    assert(!test_relay_step(t));
  assert(t->r.frame_ready && t->yields);
  assert(t->pixels[0] >> 11 > 28);
  test_relay_free(t);
  free(data);
  puts("Relay JPEG sanitized decode and allocation cleanup passed");
  return 0;
}
