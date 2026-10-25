#ifndef DESKTOP_RFB_H
#define DESKTOP_RFB_H
#include <stddef.h>
#include <stdint.h>

/* Callbacks transfer exactly len bytes, yielding/cancelling in the platform
 * layer. */
typedef struct {
  void *context;
  int (*read)(void *, void *, size_t);
  int (*write)(void *, const void *, size_t);
  void *(*alloc)(void *, size_t);
  void (*free)(void *, void *);
} rfb_io_t;
typedef struct {
  rfb_io_t io;
  uint16_t *pixels;
  unsigned width, height;
  unsigned minor;
  unsigned resized;
  char name[128];
  const char *error;
} rfb_t;
int rfb_start(rfb_t *, const char *password);
int rfb_update(rfb_t *);
int rfb_request(rfb_t *, int incremental);
int rfb_key(rfb_t *, uint32_t keysym, int down);
int rfb_pointer(rfb_t *, unsigned x, unsigned y, unsigned buttons);
void rfb_close(rfb_t *);
void vnc_response(const char *, const uint8_t challenge[16],
                  uint8_t response[16]);
uint32_t rfb_keysym(int key);
#endif
