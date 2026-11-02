/* Experimental NetSurf gui_media adapter; a single decoder is shared. */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <libnsfb.h>
#include <libnsfb_plot.h>
#include "netsurf/media.h"
#include "netsurf/types.h"
#include "pax_inline_player.h"

struct gui_media {
    char *url;
    pax_inline_player_t *player;
    float volume;
    bool muted, loop, failed, paused;
};
static struct gui_media *active;
void pax_inline_shutdown(void)
{
    if (active && active->player) {
        pax_inline_close(active->player);
        active->player = NULL;
    }
    active = NULL;
}
extern nsfb_t *framebuffer_current_surface(void);

static struct gui_media *media_create(const char *url, const char *referer)
{
    (void)referer;
    struct gui_media *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->url = strdup(url);
    if (!m->url) { free(m); return NULL; }
    m->volume = 1.0f;
    return m;
}
static void media_destroy(struct gui_media *m)
{
    if (!m) return;
    if (m->player) pax_inline_close(m->player);
    if (active == m) active = NULL;
    free(m->url); free(m);
}
static void media_play(struct gui_media *m)
{
    if (!m) return;
    if (!m->player) {
        /* The P4 has one speaker and limited RAM. Reject concurrent players. */
        if (active && active != m) {
            if (!active->paused) { m->failed = true; return; }
            pax_inline_close(active->player);
            active->player = NULL;
            active = NULL;
        }
        m->player = pax_inline_open(m->url);
        if (!m->player) { m->failed = true; return; }
        active = m;
        pax_inline_volume(m->player, m->volume, m->muted);
        pax_inline_loop(m->player, m->loop);
    }
    m->failed = false;
    m->paused = false;
    pax_inline_play(m->player);
}
static void media_pause(struct gui_media *m)
{ if (m) { m->paused = true; if (m->player) pax_inline_pause(m->player); } }
static void media_seek(struct gui_media *m, double seconds) { if (m && m->player) pax_inline_seek(m->player, seconds); }
static void media_volume(struct gui_media *m, float v, bool muted)
{ if (m) { m->volume = v; m->muted = muted; if (m->player) pax_inline_volume(m->player, v, muted); } }
static void media_loop(struct gui_media *m, bool loop)
{ if (m) { m->loop = loop; if (m->player) pax_inline_loop(m->player, loop); } }
static void media_status(struct gui_media *m, struct gui_media_status *out)
{
    memset(out, 0, sizeof(*out));
    if (!m || m->failed) { out->state = GUI_MEDIA_ERROR; return; }
    if (!m->player) { out->state = GUI_MEDIA_READY; return; }
    pax_inline_status_t st;
    pax_inline_update(m->player, &st);
    switch (st.state) {
    case PAX_INLINE_OPENING: out->state=GUI_MEDIA_LOADING; break;
    case PAX_INLINE_READY: out->state=GUI_MEDIA_READY; break;
    case PAX_INLINE_PLAYING: out->state=GUI_MEDIA_PLAYING; break;
    case PAX_INLINE_PAUSED: out->state=GUI_MEDIA_PAUSED; break;
    case PAX_INLINE_ENDED: out->state=GUI_MEDIA_ENDED; break;
    default: out->state=GUI_MEDIA_ERROR; break;
    }
    out->position=st.position; out->duration=st.duration;
    out->width=st.width; out->height=st.height;
    out->frame_seq=st.frame_seq; out->has_audio=st.has_audio;
    if (st.state == PAX_INLINE_ENDED) m->paused = true;
}
static bool media_redraw(struct gui_media *m, const struct redraw_context *ctx,
                         const struct rect *dst, const struct rect *clip)
{
    (void)ctx;
    int sw, sh;
    const uint16_t *source = m && m->player ? pax_inline_frame(m->player, &sw, &sh) : NULL;
    int width, height, stride; enum nsfb_format_e fmt; uint8_t *buffer; nsfb_bbox_t screen_clip;
    nsfb_t *surface = framebuffer_current_surface();
    if (!source || !surface || !dst || nsfb_get_geometry(surface,&width,&height,&fmt) != 0 ||
        fmt != NSFB_FMT_RGB565 || nsfb_get_buffer(surface,&buffer,&stride) != 0 || !buffer ||
        !nsfb_plot_get_clip(surface,&screen_clip)) return false;
    int dw=dst->x1-dst->x0, dh=dst->y1-dst->y0;
    if (dw<=0 || dh<=0 || sw<=0 || sh<=0) return false;
    int x0=dst->x0, y0=dst->y0, x1=dst->x1, y1=dst->y1;
    if (x0<0) x0=0;
    if (y0<0) y0=0;
    if (x1>width) x1=width;
    if (y1>height) y1=height;
    if (x0<screen_clip.x0) x0=screen_clip.x0;
    if (y0<screen_clip.y0) y0=screen_clip.y0;
    if (x1>screen_clip.x1) x1=screen_clip.x1;
    if (y1>screen_clip.y1) y1=screen_clip.y1;
    if (clip) {
        if (x0<clip->x0) x0=clip->x0;
        if (y0<clip->y0) y0=clip->y0;
        if (x1>clip->x1) x1=clip->x1;
        if (y1>clip->y1) y1=clip->y1;
    }
    if (x0>=x1 || y0>=y1) return true;
    for (int y=y0; y<y1; ++y) {
        int sy=(int)((int64_t)(y-dst->y0)*sh/dh);
        if (dw == sw) {
            const uint16_t *row = source+(size_t)sy*sw+(x0-dst->x0);
            memcpy(buffer+(size_t)y*stride+(size_t)x0*2,row,(size_t)(x1-x0)*2);
            continue;
        }
        int64_t scaled_x = (int64_t)(x0-dst->x0)*sw;
        int sx = (int)(scaled_x/dw);
        int64_t remainder = scaled_x%dw;
        const int step = sw/dw, fraction = sw%dw;
        for (int x=x0; x<x1; ++x) {
            uint16_t pixel=source[(size_t)sy*sw+sx];
            memcpy(buffer+(size_t)y*stride+(size_t)x*2,&pixel,2);
            sx += step;
            remainder += fraction;
            if (remainder >= dw) { remainder -= dw; ++sx; }
        }
    }
    return true;
}
static struct gui_media_table media_table = {
    .create=media_create, .destroy=media_destroy, .play=media_play, .pause=media_pause,
    .seek=media_seek, .set_volume=media_volume, .set_loop=media_loop,
    .status=media_status, .redraw=media_redraw,
};
struct gui_media_table *framebuffer_media_table = &media_table;
