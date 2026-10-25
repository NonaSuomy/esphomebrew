#include "audio_buffer.h"
#define BLOCKS 128u
#define FRAMES 256u
struct audio_block {
  unsigned rate, count;
  short pcm[FRAMES * 2];
};
#define GET(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define SET(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
static void worker(void *arg) {
  audio_buffer_t *a = arg;
  const app_services_t *s = a->svc;
  unsigned rate = 0, started = 0;
  int64_t next = 0;
  short pcm[FRAMES * 2];
  while (GET(&a->run)) {
    if (GET(&a->reset)) {
      SET(&a->read_seq, GET(&a->write_seq));
      started = 0;
      SET(&a->draining, 0);
      SET(&a->reset, 0);
    }
    unsigned read = GET(&a->read_seq), available = GET(&a->write_seq) - read;
    if (!started && (!available || (available < 16 && !GET(&a->draining)))) {
      s->delay_ms(2);
      continue;
    }
    int consume = available && !GET(&a->paused);
    unsigned count = FRAMES;
    if (consume) {
      const audio_block_t *b = &a->blocks[read % BLOCKS];
      if (rate != b->rate) {
        rate = b->rate;
        s->audio_init((int)rate);
        next = 0;
      }
      count = b->count;
      for (unsigned i = 0; i < count * 2; i++)
        pcm[i] = b->pcm[i];
    } else {
      for (unsigned i = 0; i < FRAMES * 2; i++)
        pcm[i] = 0;
    }
    if (GET(&a->muted))
      for (unsigned i = 0; i < count * 2; i++)
        pcm[i] = 0;
    if (!rate) {
      s->delay_ms(2);
      continue;
    }
    started = 1;
    s->audio_submit(pcm, (int)count);
    int64_t now = s->get_time_us();
    if (!next || now - next > 100000)
      next = now;
    next += (int64_t)count * 1000000 / rate;
    while (GET(&a->run) && !GET(&a->reset) && s->get_time_us() < next)
      s->delay_ms(1);
    if (consume)
      SET(&a->read_seq, read + 1);
  }
  SET(&a->done, 1);
  /* The owner deletes the task before freeing its ring or unloading code. */
  for (;;)
    s->delay_ms(20);
}
int audio_buffer_open(audio_buffer_t *a, const app_services_t *s) {
  unsigned char *p = (unsigned char *)a;
  for (size_t i = 0; i < sizeof *a; i++)
    p[i] = 0;
  a->svc = s;
  if (!s->task_create || !s->task_delete || !s->audio_init || !s->audio_submit)
    return -1;
  a->blocks = s->mem_alloc(BLOCKS * sizeof *a->blocks);
  if (!a->blocks)
    return -1;
  SET(&a->run, 1);
  if (s->task_create(worker, "desktop_audio", 4096, a, 7, &a->task, 1) != 0 ||
      !a->task) {
    s->mem_free(a->blocks);
    a->blocks = 0;
    return -1;
  }
  return 0;
}
int audio_buffer_write(audio_buffer_t *a, const uint8_t *pcm, size_t n,
                       unsigned channels, unsigned rate, int (*cancel)(void *),
                       void *ctx) {
  if (!a->blocks || (channels != 1 && channels != 2) || rate < 8000 ||
      rate > 48000 || n % (channels * 2))
    return -1;
  unsigned frame = channels * 2;
  while (n) {
    unsigned write = GET(&a->write_seq);
    while (write - GET(&a->read_seq) >= BLOCKS || GET(&a->reset)) {
      if (cancel && cancel(ctx))
        return -1;
      a->svc->delay_ms(2);
    }
    if (cancel && cancel(ctx))
      return -1;
    audio_block_t *b = &a->blocks[write % BLOCKS];
    b->rate = rate;
    b->count = n / frame < FRAMES ? (unsigned)(n / frame) : FRAMES;
    for (unsigned i = 0; i < b->count; i++) {
      const uint8_t *p = pcm + i * frame;
      b->pcm[2 * i] = (int16_t)(p[0] | (unsigned)p[1] << 8);
      b->pcm[2 * i + 1] =
          channels == 2 ? (int16_t)(p[2] | (unsigned)p[3] << 8) : b->pcm[2 * i];
    }
    n -= b->count * frame;
    pcm += b->count * frame;
    SET(&a->write_seq, write + 1);
  }
  return 0;
}
void audio_buffer_pause(audio_buffer_t *a, int value) {
  SET(&a->paused, !!value);
}
void audio_buffer_mute(audio_buffer_t *a, int value) {
  SET(&a->muted, !!value);
}
void audio_buffer_finish(audio_buffer_t *a, int drain) {
  if (!a->task)
    return;
  SET(&a->paused, 0);
  if (drain) {
    SET(&a->draining, 1);
    int64_t deadline = a->svc->get_time_us() + 4000000;
    while (GET(&a->read_seq) != GET(&a->write_seq) &&
           a->svc->get_time_us() < deadline)
      a->svc->delay_ms(2);
  }
  SET(&a->reset, 1);
  int64_t deadline = a->svc->get_time_us() + 1000000;
  while (GET(&a->reset) && a->svc->get_time_us() < deadline)
    a->svc->delay_ms(2);
}
void audio_buffer_close(audio_buffer_t *a) {
  if (!a->task)
    return;
  SET(&a->run, 0);
  int64_t deadline = a->svc->get_time_us() + 1000000;
  while (!GET(&a->done) && a->svc->get_time_us() < deadline)
    a->svc->delay_ms(2);
  a->svc->task_delete(a->task);
  a->task = 0;
  a->svc->mem_free(a->blocks);
  a->blocks = 0;
}
