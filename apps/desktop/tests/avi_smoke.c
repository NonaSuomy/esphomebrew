/* Sanitized complete AVI parsing, JPEG decode, sound, EOF and cleanup. */
#include "relay_bridge.c"
#include <stdio.h>
int main(int argc, char **argv) {
  assert(argc == 2);
  FILE *f = fopen(argv[1], "rb"); assert(f);
  assert(!fseek(f, 0, SEEK_END)); long n = ftell(f); assert(n > 0); rewind(f);
  unsigned char *data = malloc((size_t)n); assert(data);
  assert(fread(data, 1, (size_t)n, f) == (size_t)n); fclose(f);
  test_t *t = test_relay_new(64, 48);
  test_relay_feed(t, data, (size_t)n); test_relay_eof(t);
  int result = 0;
  for (int i = 0; i < 20000 && !result; i++) {
    result = test_relay_step(t); t->now += 1000;
  }
  assert(result == 1 && t->r.picture_received && t->yields);
  assert(t->audio_size == 48000 * 4 && t->audio_rate == 48000);
  assert(t->sent_len >= 2 && t->sent[t->sent_len - 2] == 'E' && t->sent[t->sent_len - 1] == 1);
  test_relay_free(t); free(data);
  puts("AVI sanitized: demux, JPEG, PCM, completion and cleanup passed");
  return 0;
}
