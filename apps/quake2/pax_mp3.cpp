/* C ABI for the Apache-2.0 PacketVideo fixed-point decoder already used by
 * the Quake PAX. Allocate directly through services: the engine's newlib
 * allocation tracker is single-threaded and must not be used by this task. */
#ifndef _Static_assert
#define _Static_assert static_assert
#endif
#include "psram_app.h"
#include "pvmp3_framedecoder.h"
#include "pvmp3decoder_api.h"
#include <stdint.h>
#include <string.h>

extern "C" {
extern const app_services_t *_pax_svc;
struct pax_mp3_decoder {
    void *memory;
    tPVMP3DecoderExternal ext;
};
void pax_mp3_reset(void *opaque) {
    pax_mp3_decoder *d = static_cast<pax_mp3_decoder *>(opaque);
    if (!d) return;
    memset(&d->ext, 0, sizeof d->ext);
    d->ext.equalizerType = flat;
    d->ext.crcEnabled = 0;
    pvmp3_InitDecoder(&d->ext, d->memory);
}
void *pax_mp3_create(void) {
    pax_mp3_decoder *d = static_cast<pax_mp3_decoder *>(_pax_svc->mem_calloc(1, sizeof(*d)));
    if (!d) return NULL;
    d->memory = _pax_svc->mem_calloc(1, pvmp3_decoderMemRequirements());
    if (!d->memory) { _pax_svc->mem_free(d); return NULL; }
    pax_mp3_reset(d);
    return d;
}
void pax_mp3_destroy(void *opaque) {
    pax_mp3_decoder *d = static_cast<pax_mp3_decoder *>(opaque);
    if (!d) return;
    _pax_svc->mem_free(d->memory);
    _pax_svc->mem_free(d);
}
int pax_mp3_decode(void *opaque, const uint8_t *input, int length,
                    int16_t *output, int capacity, int *used, int *samples,
                    int *rate, int *channels) {
    pax_mp3_decoder *d = static_cast<pax_mp3_decoder *>(opaque);
    if (!d || !input || length <= 0 || !output || capacity <= 0) return -1;
    d->ext.pInputBuffer = const_cast<uint8_t *>(input);
    d->ext.inputBufferCurrentLength = length;
    d->ext.inputBufferMaxLength = length;
    d->ext.inputBufferUsedLength = 0;
    d->ext.pOutputBuffer = output;
    d->ext.outputFrameSize = capacity;
    int result = pvmp3_framedecoder(&d->ext, d->memory);
    *used = d->ext.inputBufferUsedLength;
    *samples = d->ext.outputFrameSize;
    *rate = d->ext.samplingRate;
    *channels = d->ext.num_channels;
    return result;
}
}
