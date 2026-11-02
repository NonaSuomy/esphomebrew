/* PAX save states, adapted from HuExpress's ESP32 port. */
#include "runtime.h"
#include "pce.h"
#include "gfx.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define PCE_STATE_MAGIC 0x50434553u
#define PCE_STATE_VER 4u
#define STATE_CAP (256u * 1024u)
typedef struct { unsigned char *data; size_t pos, len; } state_stream;
static bool wr(state_stream *f, const void *data, size_t len) {
 if (len > STATE_CAP - f->pos) return false;
 memcpy(f->data + f->pos, data, len); f->pos += len; return true;
}
static bool rd(state_stream *f, void *data, size_t len) {
 if (len > f->len - f->pos) return false;
 memcpy(data, f->data + f->pos, len); f->pos += len; return true;
}
static uint32_t state_hash(const unsigned char *p, size_t n) {
 uint32_t h=2166136261u; while (n--) h=(h ^ *p++)*16777619u; return h;
}
static void state_path(char *out, size_t cap, uint32_t rom, unsigned slot, bool temp) {
 snprintf(out,cap,"/sd/roms/pax/pce-%08lx-%u.state%s",(unsigned long)rom,slot,temp?".tmp":"");
}
bool pce_state_save(uint32_t rom, unsigned slot) {
 if (!_pax_svc->file_open || !_pax_svc->file_write || !_pax_svc->file_close || !_pax_svc->file_rename || !_pax_svc->file_stat || !_pax_svc->file_remove) return false;
 unsigned char *data=malloc(STATE_CAP); if (!data) return false;
 state_stream stream={data,0,0}; state_stream *f=&stream;
    uint32_t magic = PCE_STATE_MAGIC;
    uint32_t ver   = PCE_STATE_VER;
    bool ok = true;

    ok = ok && wr(f, &magic, 4);
    ok = ok && wr(f, &ver, 4);

    /* CPU registers — use the actual engine globals, not the struct
       fields (which are NOT aliased on this platform).  reg_pc is
       uint32 in the global but we store 16-bit in the file. */
    { uint16_t pc16 = (uint16_t)reg_pc;
      ok = ok && wr(f, &pc16, 2); }
    { uchar ra = reg_a; ok = ok && wr(f, &ra, 1); }
    { uchar rx = reg_x; ok = ok && wr(f, &rx, 1); }
    { uchar ry = reg_y; ok = ok && wr(f, &ry, 1); }
    { uchar rp = reg_p; ok = ok && wr(f, &rp, 1); }
    { uchar rs = reg_s; ok = ok && wr(f, &rs, 1); }

    /* Cycle counters — cyclecount/cyclecountold/scanline ARE aliased
       via pointers, but cycles_ is a standalone global */
    ok = ok && wr(f, &hard_pce->s_cyclecount,    4);
    ok = ok && wr(f, &hard_pce->s_cyclecountold,  4);
    ok = ok && wr(f, &cycles_,                    4);
    ok = ok && wr(f, &hard_pce->s_scanline,       4);

    /* MMR */
    ok = ok && wr(f, hard_pce->mmr, 8);

    /* RAM, WRAM, VRAM */
    ok = ok && wr(f, hard_pce->RAM,  0x8000);
    ok = ok && wr(f, hard_pce->WRAM, 0x2000);
    ok = ok && wr(f, hard_pce->VRAM, VRAMSIZE);

    /* SPRAM, Pal */
    ok = ok && wr(f, hard_pce->SPRAM, 64 * 4 * sizeof(uint16));
    ok = ok && wr(f, hard_pce->Pal,   512);

    /* vchange / vchanges */
    ok = ok && wr(f, hard_pce->vchange,  VRAMSIZE / 32);
    ok = ok && wr(f, hard_pce->vchanges, VRAMSIZE / 128);

    /* IO: VCE palette */
    ok = ok && wr(f, hard_pce->s_io.VCE, 0x200 * sizeof(pair));

    /* IO: VCE register + ratch */
    ok = ok && wr(f, &hard_pce->s_io.vce_reg,   sizeof(pair));
    ok = ok && wr(f, &hard_pce->s_io.vce_ratch, 1);

    /* IO: VDC registers (array in io struct — kept for compat) */
    ok = ok && wr(f, hard_pce->s_io.VDC, 32 * sizeof(pair));

    /* IO: VDC standalone vars (MY_VDC_VARS — the ACTUAL live registers)
       Patch IO_VDC_05_CR before writing: during VBLANK the game may have
       zeroed CR, so use pce_display_cr captured at display start. */
    pair saved_vdc05_standalone = IO_VDC_05_CR;
    if (pce_display_cr)
        IO_VDC_05_CR.W = pce_display_cr;

    ok = ok && wr(f, &IO_VDC_00_MAWR,  sizeof(pair));
    ok = ok && wr(f, &IO_VDC_01_MARR,  sizeof(pair));
    ok = ok && wr(f, &IO_VDC_02_VWR,   sizeof(pair));
    ok = ok && wr(f, &IO_VDC_03_vdc3,  sizeof(pair));
    ok = ok && wr(f, &IO_VDC_04_vdc4,  sizeof(pair));
    ok = ok && wr(f, &IO_VDC_05_CR,    sizeof(pair));
    ok = ok && wr(f, &IO_VDC_06_RCR,   sizeof(pair));
    ok = ok && wr(f, &IO_VDC_07_BXR,   sizeof(pair));
    ok = ok && wr(f, &IO_VDC_08_BYR,   sizeof(pair));
    ok = ok && wr(f, &IO_VDC_09_MWR,   sizeof(pair));
    ok = ok && wr(f, &IO_VDC_0A_HSR,   sizeof(pair));
    ok = ok && wr(f, &IO_VDC_0B_HDR,   sizeof(pair));
    ok = ok && wr(f, &IO_VDC_0C_VPR,   sizeof(pair));
    ok = ok && wr(f, &IO_VDC_0D_VDW,   sizeof(pair));
    ok = ok && wr(f, &IO_VDC_0E_VCR,   sizeof(pair));
    ok = ok && wr(f, &IO_VDC_0F_DCR,   sizeof(pair));
    ok = ok && wr(f, &IO_VDC_10_SOUR,  sizeof(pair));
    ok = ok && wr(f, &IO_VDC_11_DISTR, sizeof(pair));
    ok = ok && wr(f, &IO_VDC_12_LENR,  sizeof(pair));
    ok = ok && wr(f, &IO_VDC_13_SATB,  sizeof(pair));
    ok = ok && wr(f, &IO_VDC_14,       sizeof(pair));

    /* Restore live IO_VDC_05_CR so emulation continues unaffected */
    IO_VDC_05_CR = saved_vdc05_standalone;

    ok = ok && wr(f, &hard_pce->s_io.vdc_inc,          2);
    ok = ok && wr(f, &hard_pce->s_io.vdc_raster_count, 2);
    ok = ok && wr(f, &hard_pce->s_io.vdc_reg,          1);
    ok = ok && wr(f, &hard_pce->s_io.vdc_status,       1);
    ok = ok && wr(f, &hard_pce->s_io.vdc_ratch,        1);
    ok = ok && wr(f, &hard_pce->s_io.vdc_satb,         1);
    ok = ok && wr(f, &hard_pce->s_io.vdc_pendvsync,    1);
    ok = ok && wr(f, &hard_pce->s_io.bg_h,             4);
    ok = ok && wr(f, &hard_pce->s_io.bg_w,             4);
    ok = ok && wr(f, &hard_pce->s_io.screen_w,         4);
    ok = ok && wr(f, &hard_pce->s_io.screen_h,         4);
    ok = ok && wr(f, &hard_pce->s_io.scroll_y,         4);
    ok = ok && wr(f, &hard_pce->s_io.minline,          4);
    ok = ok && wr(f, &hard_pce->s_io.maxline,          4);
    ok = ok && wr(f, &hard_pce->s_io.vdc_min_display,  2);
    ok = ok && wr(f, &hard_pce->s_io.vdc_max_display,  2);

    /* IO: Joypad */
    ok = ok && wr(f, hard_pce->s_io.JOY, 16);
    ok = ok && wr(f, &hard_pce->s_io.joy_select,  1);
    ok = ok && wr(f, &hard_pce->s_io.joy_counter, 1);

    /* IO: PSG */
    ok = ok && wr(f, hard_pce->s_io.PSG,  sizeof(hard_pce->s_io.PSG));
    ok = ok && wr(f, hard_pce->s_io.wave, sizeof(hard_pce->s_io.wave));
    ok = ok && wr(f, &hard_pce->s_io.psg_ch,        1);
    ok = ok && wr(f, &hard_pce->s_io.psg_volume,    1);
    ok = ok && wr(f, &hard_pce->s_io.psg_lfo_freq,  1);
    ok = ok && wr(f, &hard_pce->s_io.psg_lfo_ctrl,  1);
    for (int i = 0; i < 6; i++)
        ok = ok && wr(f, hard_pce->s_io.psg_da_data[i], PSG_DIRECT_ACCESS_BUFSIZE);
    ok = ok && wr(f, hard_pce->s_io.psg_da_index, sizeof(hard_pce->s_io.psg_da_index));
    ok = ok && wr(f, hard_pce->s_io.psg_da_count, sizeof(hard_pce->s_io.psg_da_count));

    /* IO: Timer, IRQ */
    ok = ok && wr(f, &hard_pce->s_io.timer_reload,  1);
    ok = ok && wr(f, &hard_pce->s_io.timer_start,   1);
    ok = ok && wr(f, &hard_pce->s_io.timer_counter, 1);
    ok = ok && wr(f, &hard_pce->s_io.irq_mask,      1);
    ok = ok && wr(f, &hard_pce->s_io.irq_status,    1);

    /* IO: io_buffer */
    ok = ok && wr(f, &hard_pce->s_io.io_buffer, 1);

    /* GFX context — the rendering engine caches VDC control register
       (background/sprite enable bits) in saved_gfx_context[].cr.
       The save may happen during VBLANK when the game has temporarily
       disabled BG/sprites (e.g. for DMA).  Use pce_display_cr which
       was captured at the start of the visible area instead. */
    {
        uint16 orig_ctx_cr   = saved_gfx_context[0].cr;
        pair   orig_vdc5_io  = hard_pce->s_io.VDC[5];

        if (pce_display_cr)
            saved_gfx_context[0].cr = pce_display_cr;
        hard_pce->s_io.VDC[5].W = saved_gfx_context[0].cr;

        ok = ok && wr(f, saved_gfx_context,
                      sizeof(gfx_context) * MAX_GFX_CONTEXT_SLOT_NUMBER);
        ok = ok && wr(f, &gfx_need_redraw, sizeof(gfx_need_redraw));

        /* Restore live state so emulation is unaffected */
        saved_gfx_context[0].cr = orig_ctx_cr;
        hard_pce->s_io.VDC[5]  = orig_vdc5_io;
    }


 if (ok && f->pos <= STATE_CAP-4) { uint32_t hash=state_hash(data,f->pos); ok=wr(f,&hash,4); } else ok=false;
 char path[96],tmp[96]; state_path(path,sizeof(path),rom,slot,false); state_path(tmp,sizeof(tmp),rom,slot,true);
 void *file=ok?_pax_svc->file_open(tmp,"wb"):NULL;
 if (!file) ok=false;
 if (file) { if (_pax_svc->file_write(data,1,f->pos,file)!=f->pos) ok=false; if (_pax_svc->file_close(file)!=0) ok=false; }
 /* The loader deliberately refuses to rename over an existing file. Move
  * the old save aside first and restore it if installing the new one fails. */
 char backup[104]; snprintf(backup,sizeof(backup),"%s.bak",path);
 pax_file_stat_t statbuf; bool had_old=false;
 if (ok && _pax_svc->file_stat(path,&statbuf)==0) {
   if (_pax_svc->file_stat(backup,&statbuf)==0) ok=false;
   else { had_old=_pax_svc->file_rename(path,backup)==0; if (!had_old) ok=false; }
 }
 if (ok && _pax_svc->file_rename(tmp,path)!=0) { ok=false; if (had_old) _pax_svc->file_rename(backup,path); }
 if (ok && had_old) _pax_svc->file_remove(backup);
 if (!ok) _pax_svc->file_remove(tmp);
 free(data); return ok;
}
bool pce_state_load(uint32_t rom, unsigned slot) {
 if (!_pax_svc->file_open || !_pax_svc->file_read || !_pax_svc->file_close) return false;
 char path[96]; state_path(path,sizeof(path),rom,slot,false);
 void *file=_pax_svc->file_open(path,"rb"); if (!file) return false;
 unsigned char *data=malloc(STATE_CAP); if (!data) { _pax_svc->file_close(file); return false; }
 size_t n=_pax_svc->file_read(data,1,STATE_CAP,file); _pax_svc->file_close(file);
 uint32_t magic0=0,ver0=0,hash=0; if(n>=12) { memcpy(&magic0,data,4); memcpy(&ver0,data+4,4); memcpy(&hash,data+n-4,4); }
 if(n<12 || n==STATE_CAP || magic0!=PCE_STATE_MAGIC || ver0!=PCE_STATE_VER || state_hash(data,n-4)!=hash) {free(data);return false;}
 state_stream stream={data,0,n-4}; state_stream *f=&stream;
    uint32_t magic, ver;
    bool ok = true;

    ok = ok && rd(f, &magic, 4);
    ok = ok && rd(f, &ver, 4);
    /* CPU registers — read into the actual engine globals */
    { uint16_t pc16 = 0;
      ok = ok && rd(f, &pc16, 2);
      reg_pc = (uint32)pc16; }
    { uchar ra = 0; ok = ok && rd(f, &ra, 1); reg_a = ra; }
    { uchar rx = 0; ok = ok && rd(f, &rx, 1); reg_x = rx; }
    { uchar ry = 0; ok = ok && rd(f, &ry, 1); reg_y = ry; }
    { uchar rp = 0; ok = ok && rd(f, &rp, 1); reg_p = rp; }
    { uchar rs = 0; ok = ok && rd(f, &rs, 1); reg_s = rs; }

    /* Cycle counters */
    ok = ok && rd(f, &hard_pce->s_cyclecount,    4);
    ok = ok && rd(f, &hard_pce->s_cyclecountold,  4);
    ok = ok && rd(f, &cycles_,                    4);
    ok = ok && rd(f, &hard_pce->s_scanline,       4);

    /* MMR */
    ok = ok && rd(f, hard_pce->mmr, 8);

    /* RAM, WRAM, VRAM */
    ok = ok && rd(f, hard_pce->RAM,  0x8000);
    ok = ok && rd(f, hard_pce->WRAM, 0x2000);
    ok = ok && rd(f, hard_pce->VRAM, VRAMSIZE);

    /* SPRAM, Pal */
    ok = ok && rd(f, hard_pce->SPRAM, 64 * 4 * sizeof(uint16));
    ok = ok && rd(f, hard_pce->Pal,   512);

    /* vchange / vchanges */
    ok = ok && rd(f, hard_pce->vchange,  VRAMSIZE / 32);
    ok = ok && rd(f, hard_pce->vchanges, VRAMSIZE / 128);

    /* IO: VCE palette */
    ok = ok && rd(f, hard_pce->s_io.VCE, 0x200 * sizeof(pair));

    /* IO: VCE register + ratch */
    ok = ok && rd(f, &hard_pce->s_io.vce_reg,   sizeof(pair));
    ok = ok && rd(f, &hard_pce->s_io.vce_ratch, 1);

    /* IO: VDC registers (array in io struct — kept for compat) */
    ok = ok && rd(f, hard_pce->s_io.VDC, 32 * sizeof(pair));

    /* IO: VDC standalone vars (MY_VDC_VARS) */
    ok = ok && rd(f, &IO_VDC_00_MAWR,  sizeof(pair));
    ok = ok && rd(f, &IO_VDC_01_MARR,  sizeof(pair));
    ok = ok && rd(f, &IO_VDC_02_VWR,   sizeof(pair));
    ok = ok && rd(f, &IO_VDC_03_vdc3,  sizeof(pair));
    ok = ok && rd(f, &IO_VDC_04_vdc4,  sizeof(pair));
    ok = ok && rd(f, &IO_VDC_05_CR,    sizeof(pair));
    ok = ok && rd(f, &IO_VDC_06_RCR,   sizeof(pair));
    ok = ok && rd(f, &IO_VDC_07_BXR,   sizeof(pair));
    ok = ok && rd(f, &IO_VDC_08_BYR,   sizeof(pair));
    ok = ok && rd(f, &IO_VDC_09_MWR,   sizeof(pair));
    ok = ok && rd(f, &IO_VDC_0A_HSR,   sizeof(pair));
    ok = ok && rd(f, &IO_VDC_0B_HDR,   sizeof(pair));
    ok = ok && rd(f, &IO_VDC_0C_VPR,   sizeof(pair));
    ok = ok && rd(f, &IO_VDC_0D_VDW,   sizeof(pair));
    ok = ok && rd(f, &IO_VDC_0E_VCR,   sizeof(pair));
    ok = ok && rd(f, &IO_VDC_0F_DCR,   sizeof(pair));
    ok = ok && rd(f, &IO_VDC_10_SOUR,  sizeof(pair));
    ok = ok && rd(f, &IO_VDC_11_DISTR, sizeof(pair));
    ok = ok && rd(f, &IO_VDC_12_LENR,  sizeof(pair));
    ok = ok && rd(f, &IO_VDC_13_SATB,  sizeof(pair));
    ok = ok && rd(f, &IO_VDC_14,       sizeof(pair));
    ok = ok && rd(f, &hard_pce->s_io.vdc_inc,          2);
    ok = ok && rd(f, &hard_pce->s_io.vdc_raster_count, 2);
    ok = ok && rd(f, &hard_pce->s_io.vdc_reg,          1);
    ok = ok && rd(f, &hard_pce->s_io.vdc_status,       1);
    ok = ok && rd(f, &hard_pce->s_io.vdc_ratch,        1);
    ok = ok && rd(f, &hard_pce->s_io.vdc_satb,         1);
    ok = ok && rd(f, &hard_pce->s_io.vdc_pendvsync,    1);
    ok = ok && rd(f, &hard_pce->s_io.bg_h,             4);
    ok = ok && rd(f, &hard_pce->s_io.bg_w,             4);
    ok = ok && rd(f, &hard_pce->s_io.screen_w,         4);
    ok = ok && rd(f, &hard_pce->s_io.screen_h,         4);
    ok = ok && rd(f, &hard_pce->s_io.scroll_y,         4);
    ok = ok && rd(f, &hard_pce->s_io.minline,          4);
    ok = ok && rd(f, &hard_pce->s_io.maxline,          4);
    ok = ok && rd(f, &hard_pce->s_io.vdc_min_display,  2);
    ok = ok && rd(f, &hard_pce->s_io.vdc_max_display,  2);

    /* IO: Joypad */
    ok = ok && rd(f, hard_pce->s_io.JOY, 16);
    ok = ok && rd(f, &hard_pce->s_io.joy_select,  1);
    ok = ok && rd(f, &hard_pce->s_io.joy_counter, 1);

    /* IO: PSG */
    ok = ok && rd(f, hard_pce->s_io.PSG,  sizeof(hard_pce->s_io.PSG));
    ok = ok && rd(f, hard_pce->s_io.wave, sizeof(hard_pce->s_io.wave));
    ok = ok && rd(f, &hard_pce->s_io.psg_ch,        1);
    ok = ok && rd(f, &hard_pce->s_io.psg_volume,    1);
    ok = ok && rd(f, &hard_pce->s_io.psg_lfo_freq,  1);
    ok = ok && rd(f, &hard_pce->s_io.psg_lfo_ctrl,  1);
    for (int i = 0; i < 6; i++)
        ok = ok && rd(f, hard_pce->s_io.psg_da_data[i], PSG_DIRECT_ACCESS_BUFSIZE);
    ok = ok && rd(f, hard_pce->s_io.psg_da_index, sizeof(hard_pce->s_io.psg_da_index));
    ok = ok && rd(f, hard_pce->s_io.psg_da_count, sizeof(hard_pce->s_io.psg_da_count));

    /* IO: Timer, IRQ */
    ok = ok && rd(f, &hard_pce->s_io.timer_reload,  1);
    ok = ok && rd(f, &hard_pce->s_io.timer_start,   1);
    ok = ok && rd(f, &hard_pce->s_io.timer_counter, 1);
    ok = ok && rd(f, &hard_pce->s_io.irq_mask,      1);
    ok = ok && rd(f, &hard_pce->s_io.irq_status,    1);

    /* IO: io_buffer */
    ok = ok && rd(f, &hard_pce->s_io.io_buffer, 1);

    /* GFX context */
    ok = ok && rd(f, saved_gfx_context,
                  sizeof(gfx_context) * MAX_GFX_CONTEXT_SLOT_NUMBER);
    ok = ok && rd(f, &gfx_need_redraw, sizeof(gfx_need_redraw));


 if (!ok || f->pos != f->len) { free(data); return false; }
 free(data);
    /* Restore bank mappings from MMR values */
    for (int i = 0; i < 8; i++)
        bank_set(i, hard_pce->mmr[i]);

    /* Restore IO_VDC_active_ref from saved vdc_reg */
    IO_VDC_active_set(io.vdc_reg);

    /* Derive bg_w / bg_h from restored MWR register */
    {
        static const uchar bgw[] = { 32, 64, 128, 128 };
        uchar mwr_lo = IO_VDC_09_MWR.B.l;
        io.bg_h = (mwr_lo & 0x40) ? 64 : 32;
        io.bg_w = bgw[(mwr_lo >> 4) & 3];
    }

    /* Derive vdc_inc from restored CR high byte */
    {
        static const uchar incsize[] = { 1, 32, 64, 128 };
        io.vdc_inc = incsize[(IO_VDC_05_CR.B.h >> 3) & 3];
    }

    /* Derive screen_w from restored HDR register */
    io.screen_w = (IO_VDC_0B_HDR.B.l + 1) * 8;

    /* Push the cached VDC control register back into the standalone var.
       The render loop uses saved_gfx_context[0].cr as the active CR. */
    IO_VDC_05_CR.W = saved_gfx_context[0].cr;

    /* Trigger video mode recalc on next frame */
    gfx_need_video_mode_change = 1;

    /* Force full tile-cache rebuild: mark every tile as changed so
       the engine regenerates VRAM2 (decoded tiles) from restored VRAM */
    memset(hard_pce->vchange,  1, VRAMSIZE / 32);
    memset(hard_pce->vchanges, 1, VRAMSIZE / 128);

    /* Rebuild palette */
    SetPalette();

    /* Protect the restored CR value against the game's VBLANK code
       that may zero VDC5 before the display area on the first few frames. */
    pce_resume_cr_value   = saved_gfx_context[0].cr;
    pce_resume_cr_protect = 120;   /* ~2 seconds at 60fps */

    return true;
}
bool pce_state_delete(uint32_t rom, unsigned slot) {
 if (!_pax_svc->file_remove) return false; char path[96]; state_path(path,sizeof(path),rom,slot,false);
 return _pax_svc->file_remove(path)==0;
}
