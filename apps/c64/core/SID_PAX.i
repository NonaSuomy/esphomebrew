/* Synchronous SID output adapter for a PSRAM-loaded PAX. */

#include "psram_app.h"
#include "VIC.h"
#include <string.h>

#define PAX_AUDIO_FRAMES 256

static int16_t pax_sid_mono[PAX_AUDIO_FRAMES];
static int16_t pax_sid_stereo[PAX_AUDIO_FRAMES * 2];
static int pax_sid_pending;
static bool pax_sid_paused;
static int pax_sid_divisor;

void DigitalRenderer::init_sound(void)
{
    pax_sid_pending = 0;
    pax_sid_divisor = 0;
    pax_sid_paused = false;
    ready = true;
    if (_pax_svc && _pax_svc->audio_init)
        _pax_svc->audio_init((int)SAMPLE_FREQ);
}

void DigitalRenderer::EmulateLine(void)
{
    if (!ready || pax_sid_paused) return;

    pax_sid_divisor += SAMPLE_FREQ;
    while (pax_sid_divisor >= 0) {
        pax_sid_divisor -= TOTAL_RASTERS * SCREEN_FREQ;
        ++pax_sid_pending;
    }

    sample_buf[sample_in_ptr] = volume;
    sample_in_ptr = (sample_in_ptr + 1) % SAMPLE_BUF_SIZE;

    if (pax_sid_pending >= PAX_AUDIO_FRAMES) {
        pax_sid_pending -= PAX_AUDIO_FRAMES;
        calc_buffer(pax_sid_mono, PAX_AUDIO_FRAMES * 2);

        /* Frodo's original renderer includes the SID's volume-dependent DC
         * offset in every sample.  That is useful for an analogue SID model,
         * but it becomes an audible constant buzz when the voices are idle
         * and the samples are sent to a digital I2S amplifier.  Keep the
         * rendered waveform for active voices, but emit true digital silence
         * while the SID has no envelope currently producing sound. */
        int16_t block_min = pax_sid_mono[0];
        int16_t block_max = pax_sid_mono[0];
        for (int i = 1; i < PAX_AUDIO_FRAMES; ++i) {
            if (pax_sid_mono[i] < block_min)
                block_min = pax_sid_mono[i];
            if (pax_sid_mono[i] > block_max)
                block_max = pax_sid_mono[i];
        }
        // A silent SID block is commonly a constant DC value (for example
        // 8192), not literal zero, so checking only the absolute peak is not
        // sufficient.  A small threshold avoids waking the speaker for a
        // negligible filter residue while preserving quiet real waveforms.
        const bool has_waveform = (static_cast<int>(block_max) - static_cast<int>(block_min)) > 8;
        if (!has_waveform)
            memset(pax_sid_mono, 0, sizeof(pax_sid_mono));

        for (int i = 0; i < PAX_AUDIO_FRAMES; ++i) {
            pax_sid_stereo[i * 2] = pax_sid_mono[i];
            pax_sid_stereo[i * 2 + 1] = pax_sid_mono[i];
        }
        // Do not keep the speaker pipeline alive with empty startup/menu
        // blocks.  The ESPHome speaker can then remain stopped until the
        // first real SID voice starts, avoiding an initial ring-buffer write
        // while its resampler task is still coming up.
        if (has_waveform && _pax_svc && _pax_svc->audio_submit)
            _pax_svc->audio_submit(pax_sid_stereo, PAX_AUDIO_FRAMES);
    }
}

void DigitalRenderer::changeVolumeLevel(void) {}
void DigitalRenderer::Pause(void) { pax_sid_paused = true; }
void DigitalRenderer::Resume(void) { pax_sid_paused = false; }
DigitalRenderer::~DigitalRenderer(void) {}
