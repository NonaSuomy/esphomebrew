#ifndef DESKTOP_MEDIA_CLIENT_H
#define DESKTOP_MEDIA_CLIENT_H
#include "psram_app.h"
#define MEDIA_PAGE 32
#define MEDIA_ID 1025
#define MEDIA_TITLE 513
#define MEDIA_ERROR 256

typedef struct {
  char title[MEDIA_TITLE], id[MEDIA_ID], type[129], category[33];
  int directory, playable;
} media_item_t;
typedef struct {
  char title[MEDIA_TITLE];
  media_item_t items[MEDIA_PAGE];
  unsigned count, total, offset;
} media_listing_t;
int media_parse(const char *, size_t, media_listing_t *, char *);
int media_rpc(const app_services_t *, const char *, const char *,
              media_listing_t *, char *, int (*)(void));
int media_browse(const app_services_t *, const char *, unsigned,
                 media_listing_t *, char *, int (*)(void));
int media_play(const app_services_t *, const media_item_t *, int, char *,
               int (*)(void));
int media_control(const app_services_t *, const char *, char *, int (*)(void));
int media_sd_list(const app_services_t *, const char *, unsigned,
                  media_listing_t *, char *, int (*)(void));
/* 1 audio, 2 video, 0 unsupported (folders are checked separately). */
int media_kind(const media_item_t *);
#endif
