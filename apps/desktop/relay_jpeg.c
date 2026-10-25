#include "jpeg/tjpgd.h"
#include "relay.h"

typedef struct {
  relay_t *relay;
  const uint8_t *data;
  size_t size, position;
  unsigned x, y;
} jpeg_source_t;
static size_t jpeg_read(JDEC *decoder, uint8_t *out, size_t length) {
  jpeg_source_t *s = decoder->device;
  if (length > s->size - s->position)
    length = s->size - s->position;
  if (out)
    for (size_t i = 0; i < length; i++)
      out[i] = s->data[s->position + i];
  s->position += length;
  return length;
}
static int jpeg_output(JDEC *decoder, void *rgb, JRECT *rect) {
  jpeg_source_t *s = decoder->device;
  relay_t *r = s->relay;
  unsigned w = rect->right - rect->left + 1;
  const uint16_t *in = rgb;
  for (unsigned y = rect->top; y <= rect->bottom; y++) {
    uint16_t *out =
        r->pixels + (size_t)(s->y + y) * r->width + s->x + rect->left;
    for (unsigned x = 0; x < w; x++)
      out[x] = *in++;
  }
  return !r->io.yield || !r->io.yield(r->io.context);
}
int relay_jpeg(relay_t *r, const uint8_t *data, size_t n, unsigned x,
               unsigned y, unsigned w, unsigned h) {
  if (!w || !h || x > r->width || y > r->height || w > r->width - x ||
      h > r->height - y)
    return -1;
  jpeg_source_t source = {r, data, n, 0, x, y};
  JDEC decoder;
  JRESULT result =
      jd_prepare(&decoder, jpeg_read, r->jpeg_work, 16384, &source);
  if (result != JDR_OK || decoder.width != w || decoder.height != h)
    return -1;
  return jd_decomp(&decoder, jpeg_output, 0) == JDR_OK ? 0 : -1;
}
