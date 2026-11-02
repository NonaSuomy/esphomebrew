#!/usr/bin/env python3
"""Exercise the actual PCE frame callback, including transparent-frame reset."""
from pathlib import Path
import subprocess, tempfile
source=(Path(__file__).resolve().parents[1]/'pax_main.c').read_text()
start=source.index('static void pce_gfx_draw(void)')
end=source.index('\nosd_gfx_driver osd_gfx_driver_list',start)
part=source[start:end]
prefix=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define PCE_OUTPUT_W 256
#define PCE_OUTPUT_H 240
#define XBUF_WIDTH 600
#define XBUF_HEIGHT 368
#define PCE_INDEX_STRIDE XBUF_WIDTH
static bool skipNextFrame;
static unsigned pce_video_mode=0,pce_skip_remaining,pce_skip_pattern,pce_zoom=1;
static struct {int screen_w,screen_h;} io;
static unsigned char *pce_xbuf,*pce_spm_raw,*osd_gfx_buffer,*Pal;
static uint16_t *pce_palette,*pce_pixels;
static unsigned pce_frame_count,pce_changes,pce_last_hash;
static int pce_audio_peak;
static int64_t pce_next_deadline,pce_fps_start,clock_us;
static int draws, samples; static uint16_t first,third;
static void display(const uint16_t *p,int w,int h,float scale,int overlay)
{(void)scale;(void)overlay;assert(w==256&&h==240);++draws;if(draws==1)first=p[0];if(draws==2)third=p[0];}
static int64_t get_time(void){return clock_us+=17000;}
static void delay(int ms){(void)ms;}
static void log_printf(const char *f,...){(void)f;}
static struct {void (*display_write_frame_custom)(const uint16_t*,int,int,float,int);int64_t(*get_time_us)(void);void(*delay_ms)(int);void(*log_printf)(const char*,...);} svc={display,get_time,delay,log_printf};
static __typeof__(svc) *_pax_svc=&svc;
static void pce_emit_audio(void){++samples;}
static unsigned pce_frame_hash(void){return pce_pixels[0];}
'''
suffix=r'''
int main(void){
 pce_xbuf=calloc(XBUF_WIDTH*XBUF_HEIGHT,1);pce_spm_raw=malloc(XBUF_WIDTH*XBUF_HEIGHT);
 pce_pixels=calloc(256*240,sizeof(uint16_t));pce_palette=calloc(256,sizeof(uint16_t));Pal=calloc(256,1);
 osd_gfx_buffer=pce_xbuf+32+64*XBUF_WIDTH;io.screen_w=256;io.screen_h=240;
 Pal[0]=2;pce_palette[2]=0x001f;pce_palette[3]=0xf800;osd_gfx_buffer[0]=3;
 memset(pce_spm_raw,0xff,XBUF_WIDTH*XBUF_HEIGHT);pce_gfx_draw();
 assert(draws==1 && first==0xf800 && samples==1 && skipNextFrame);
 assert(osd_gfx_buffer[0]==2 && pce_spm_raw[64*XBUF_WIDTH]==0 && pce_spm_raw[0]==0xff);
 for (int i=0;i<30;i++) pce_gfx_draw();
 assert(samples==31 && draws>5 && draws<31);
 for (unsigned mode=0;mode<3;mode++) {
  unsigned even=0,odd=0;pce_video_mode=mode;pce_frame_count=0;
  pce_skip_pattern=pce_skip_remaining=0;skipNextFrame=false;
  for (int i=0;i<30;i++) {if(!skipNextFrame) {if(i&1)odd++;else even++;}pce_gfx_draw();}
  assert(even>0 && odd>0);
  if (mode==1) assert(even>0 && odd>0);
  draws=0;
 }
 free(pce_xbuf);free(pce_spm_raw);free(pce_pixels);free(pce_palette);free(Pal);return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
 c=Path(d)/'test.c';b=Path(d)/'test';c.write_text(prefix+part+suffix)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',str(c),'-o',str(b)],check=True)
 subprocess.run([str(b)],check=True)
print('PCE frame clearing, sprite mask, presentation cadence and per-frame audio passed')
