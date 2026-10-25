#ifndef DESKTOP_AUDIO_BUFFER_H
#define DESKTOP_AUDIO_BUFFER_H
#include "psram_app.h"
#include <stddef.h>
#include <stdint.h>
typedef struct audio_block audio_block_t;
typedef struct {
  const app_services_t *svc;
  audio_block_t *blocks;
  void *task;
  unsigned read_seq, write_seq, run, done, reset, draining, paused, muted;
} audio_buffer_t;
int audio_buffer_open(audio_buffer_t *, const app_services_t *);
int audio_buffer_write(audio_buffer_t *, const uint8_t *, size_t, unsigned,
                       unsigned, int (*cancel)(void *), void *);
void audio_buffer_pause(audio_buffer_t *, int);
void audio_buffer_mute(audio_buffer_t *, int);
void audio_buffer_finish(audio_buffer_t *, int drain);
void audio_buffer_close(audio_buffer_t *);
#endif
