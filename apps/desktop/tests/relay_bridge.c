/* Opaque host-test bridge; production code has no host/network dependency. */
#include "../relay.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
  relay_t r;
  uint16_t *pixels;
  uint8_t *incoming, *sent;
  size_t incoming_len, incoming_used, sent_len;
  unsigned calls, send_calls, blocks, yields, audio_size, audio_rate,
      audio_channels;
  int64_t now;
  int eof;
} test_t;
static int receive(void *ctx, void *out, size_t n) {
  test_t *t = ctx;
  if (++t->calls % 7 == 0) return -2;
  if (t->incoming_used == t->incoming_len) return t->eof ? 0 : -2;
  if (n > 37)
    n = 37;
  if (n > t->incoming_len - t->incoming_used)
    n = t->incoming_len - t->incoming_used;
  memcpy(out, t->incoming + t->incoming_used, n);
  t->incoming_used += n;
  return (int)n;
}
static int send_data(void *ctx, const void *data, size_t n) {
  test_t *t = ctx;
  if (++t->send_calls % 5 == 0)
    return 0;
  if (n > 13)
    n = 13;
  t->sent = realloc(t->sent, t->sent_len + n);
  assert(t->sent);
  memcpy(t->sent + t->sent_len, data, n);
  t->sent_len += n;
  return (int)n;
}
static void *allocate(void *ctx, size_t n) {
  test_t *t = ctx;
  void *p = malloc(n);
  if (p)
    t->blocks++;
  return p;
}
static void release(void *ctx, void *p) {
  test_t *t = ctx;
  assert(t->blocks);
  t->blocks--;
  free(p);
}
static int64_t now(void *ctx) { return ((test_t *)ctx)->now; }
static int yield(void *ctx) {
  ((test_t *)ctx)->yields++;
  return 0;
}
static void audio(void *ctx, const uint8_t *pcm, size_t n, unsigned channels,
                  unsigned rate) {
  test_t *t = ctx;
  (void)pcm;
  t->audio_size += (unsigned)n;
  t->audio_rate = rate;
  t->audio_channels = channels;
}
void *test_relay_new(unsigned w, unsigned h) {
  test_t *t = calloc(1, sizeof *t);
  assert(t);
  t->pixels = calloc((size_t)w * h, 2);
  assert(t->pixels);
  relay_io_t io = {t, receive, send_data, allocate, release, now, yield, audio};
  assert(!relay_open(&t->r, io, t->pixels, w, h));
  return t;
}
void test_relay_free(void *ctx) {
  test_t *t = ctx;
  relay_close(&t->r);
  assert(!t->blocks);
  free(t->pixels);
  free(t->incoming);
  free(t->sent);
  free(t);
}
void test_relay_feed(void *ctx, const void *data, size_t n) {
  test_t *t = ctx;
  t->incoming = realloc(t->incoming, t->incoming_len + n);
  assert(t->incoming);
  memcpy(t->incoming + t->incoming_len, data, n);
  t->incoming_len += n;
}
int test_relay_step(void *ctx) {
  test_t *t = ctx;
  t->r.changed = 0;
  return relay_step(&t->r);
}
size_t test_relay_output(void *ctx, void *out, size_t n) {
  test_t *t = ctx;
  if (n > t->sent_len)
    n = t->sent_len;
  memcpy(out, t->sent, n);
  return t->sent_len;
}
const char *test_relay_error(void *ctx) { return ((test_t *)ctx)->r.error; }
uint16_t *test_relay_pixels(void *ctx) { return ((test_t *)ctx)->pixels; }
void test_relay_time(void *ctx, int64_t us) { ((test_t *)ctx)->now = us; }
void test_relay_cursor(void *ctx) {
  test_t *t = ctx;
  relay_draw_cursor(&t->r, t->pixels);
}
unsigned test_relay_audio_size(void *ctx) {
  return ((test_t *)ctx)->audio_size;
}
unsigned test_relay_yields(void *ctx) { return ((test_t *)ctx)->yields; }

void test_relay_eof(void *ctx) { ((test_t *)ctx)->eof = 1; }
void test_relay_pause(void *ctx, int paused) { relay_media_pause(&((test_t *)ctx)->r, paused); }
void test_relay_stop(void *ctx) { relay_media_stop(&((test_t *)ctx)->r); }
unsigned test_relay_audio_rate(void *ctx) { return ((test_t *)ctx)->audio_rate; }
unsigned test_relay_audio_channels(void *ctx) { return ((test_t *)ctx)->audio_channels; }
