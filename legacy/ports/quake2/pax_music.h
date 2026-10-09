#ifndef QUAKE2_PAX_MUSIC_H
#define QUAKE2_PAX_MUSIC_H
#include <stdint.h>
/* Called only by the steady-rate output task; never performs file I/O or
 * decoding. Music is already stereo PCM at the game's 22050 Hz rate. */
void pax_music_mix(int16_t *stereo, unsigned frames);
void pax_music_shutdown(void);
void *pax_mp3_create(void);
void pax_mp3_reset(void *decoder);
void pax_mp3_destroy(void *decoder);
int pax_mp3_decode(void *decoder, const uint8_t *input, int input_length,
                    int16_t *output, int capacity_samples, int *consumed,
                    int *samples, int *rate, int *channels);
#endif
