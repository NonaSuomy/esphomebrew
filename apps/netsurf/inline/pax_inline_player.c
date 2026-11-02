/* Experimental nonblocking interface to the existing PAX video decoder.
 * This file is compiled instead of pax_player.c, not alongside it. */
#define PAX_INLINE_VIDEO 1
#include "../../video/pax_player.c"
#include "pax_inline_player.h"

struct pax_inline_player {
    player_t *player;
    picture_t picture;
    int shown_serial;
    int64_t shown_pts;
    unsigned frame_seq;
    bool requested, loop, ended, ever_started;
    int64_t ended_at, last_stats_us;
};

pax_inline_player_t *pax_inline_open(const char *url)
{
    if (!url || !*url || strlen(url) >= PAX_APP_ARG_MAX) return NULL;
    pax_inline_player_t *in = calloc(1, sizeof(*in));
    player_t *p = calloc(1, sizeof(*p));
    if (!in || !p) { free(in); free(p); return NULL; }
    in->player = p;
    in->shown_serial = -1;
    in->shown_pts = AV_NOPTS_VALUE;
    snprintf(p->location, sizeof(p->location), "%s", url);
    title_from_location(p->title, sizeof(p->title), url);
    p->vidx = p->aidx = -1;
    p->duration_us = -1;
    p->demux_eof_serial = p->video_eof_serial = p->audio_eof_serial = -1;
    p->paused = 1;
    p->inline_gain = 256;
    p->held_slot = -1;
    p->clk_lock = pax_lock_new();
    if (!p->clk_lock || !q_init(&p->vq, VCAP) || !q_init(&p->aq, ACAP)) goto fail;
    for (int i = 0; i < NSLOTS; ++i) {
        p->slots[i].frame = av_frame_alloc();
        if (!p->slots[i].frame) goto fail;
        iq_push(&p->freeq, i);
    }
    if (pax_svc->task_create(demux_task, "ns-media", 64 * 1024, p, 4, &p->demux_task, 0) != 0) {
        p->demux_task = NULL;
        goto fail;
    }
    return in;
fail:
    player_free(p);
    free(in);
    return NULL;
}

void pax_inline_close(pax_inline_player_t *in)
{
    if (!in) return;
    player_free(in->player);
    free(in->picture.px);
    free(in);
}
void pax_inline_play(pax_inline_player_t *in)
{
    if (!in) return;
    in->requested = true;
    in->ever_started = true;
    in->ended_at = 0;
    if (in->ended) { pax_inline_seek(in, 0); in->ended = false; }
    clock_pause(in->player, false);
}
void pax_inline_pause(pax_inline_player_t *in)
{
    if (!in) return;
    in->requested = false;
    clock_pause(in->player, true);
}
void pax_inline_seek(pax_inline_player_t *in, double seconds)
{
    if (!in || seconds != seconds || seconds < 0 || seconds > 864000) return;
    player_t *p = in->player;
    if (p->state != ST_READY || !p->stream || !pax_stream_seekable(p->stream)) return;
    int64_t current = 0;
    clock_get(p, &current);
    int64_t target = (int64_t)(seconds * 1000000.0) + p->start_us;
    if (p->duration_us > 0 && target > p->start_us + p->duration_us)
        target = p->start_us + p->duration_us;
    p->seek_from = current;
    p->seek_dir = target > current ? 1 : -1;
    p->seek_target = target;
    fence();
    p->seek_req = 1;
    in->ended = false;
    in->ended_at = 0;
}
void pax_inline_volume(pax_inline_player_t *in, float volume, bool muted)
{
    if (!in) return;
    if (!(volume >= 0)) volume = 0;
    if (volume > 1) volume = 1;
    in->player->inline_gain = muted ? 0 : (int)(volume * 256);
}
void pax_inline_loop(pax_inline_player_t *in, bool loop) { if (in) in->loop = loop; }

void pax_inline_update(pax_inline_player_t *in, pax_inline_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!in) { out->state = PAX_INLINE_FAILED; return; }
    player_t *p = in->player;
    if (p->state == ST_OPENING) { out->state = PAX_INLINE_OPENING; return; }
    if (p->state == ST_FAILED) { out->state = PAX_INLINE_FAILED; return; }
    if (!p->video_task && !p->audio_task) { out->state = PAX_INLINE_FAILED; return; }
    int idx;
    while (iq_peek(&p->readyq, &idx)) {
        slot_t *slot = &p->slots[idx];
        if (slot->serial != p->serial) { iq_drop(&p->readyq); iq_push(&p->freeq, idx); continue; }
        int64_t clock = 0;
        bool have_clock = clock_get(p, &clock);
        if (have_clock && slot->pts > clock + 8000 && in->shown_serial == p->serial) break;
        if (!have_clock && (!p->actx || in->shown_serial == p->serial))
            clock_set(p, slot->pts, now_us(), p->serial);
        iq_drop(&p->readyq);
        if (convert(&in->picture, slot->frame)) {
            in->shown_pts = slot->pts;
            in->shown_serial = slot->serial;
            ++in->frame_seq;
            ++p->shown;
        }
        iq_push(&p->freeq, idx);
        break;
    }
    int serial = p->serial;
    bool done = p->demux_eof_serial == serial &&
        (!p->video_task || (p->video_eof_serial == serial && iq_count(&p->readyq) == 0)) &&
        (!p->audio_task || p->audio_eof_serial == serial);
    int64_t end_clock = 0;
    bool have_end_clock = clock_get(p, &end_clock);
    bool reached_end = pax_inline_end_due(p->demux_eof_serial == serial, have_end_clock,
                                           end_clock, p->start_us, p->duration_us);
    if ((done || reached_end) && in->requested && !p->seek_req && !p->seek_busy) {
        if (!in->ended_at) in->ended_at = now_us();
        if (reached_end || now_us() - in->ended_at > (p->audio_task ? p->lat_us : 0) + 200000) {
            if (in->loop && p->stream && pax_stream_seekable(p->stream)) pax_inline_seek(in, 0);
            else { in->ended = true; in->requested = false; clock_pause(p, true); }
        }
    } else if (!done && !reached_end) in->ended_at = 0;
    out->state = in->ended ? PAX_INLINE_ENDED : in->requested ? PAX_INLINE_PLAYING :
        (in->ever_started ? PAX_INLINE_PAUSED : PAX_INLINE_READY);
    int64_t clock = 0;
    if (!clock_get(p, &clock)) clock = in->shown_pts;
    if (clock != AV_NOPTS_VALUE && clock > p->start_us) out->position = (clock-p->start_us)/1000000.0;
    if (p->duration_us > 0) out->duration = p->duration_us/1000000.0;
    if (out->duration > 0 && out->position > out->duration) out->position = out->duration;
    int64_t now = now_us();
    if (in->requested && now - in->last_stats_us >= 3000000) {
        in->last_stats_us = now;
        pax_log("inline stats decoded=%d shown=%d dropped=%d video_queued=%u audio_queued=%u clock_ms=%lld",
                p->decoded, p->shown, p->dropped_early, q_count(&p->vq), q_count(&p->aq),
                (long long)(out->position * 1000));
    }
    out->width = p->vctx ? p->vctx->width : 0;
    out->height = p->vctx ? p->vctx->height : 0;
    out->frame_seq = in->frame_seq;
    out->has_audio = p->actx != NULL;
}
const uint16_t *pax_inline_frame(pax_inline_player_t *in, int *width, int *height)
{
    if (!in || !in->picture.px) return NULL;
    if (width) *width = in->picture.w;
    if (height) *height = in->picture.h;
    return in->picture.px;
}
