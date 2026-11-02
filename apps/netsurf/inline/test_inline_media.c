/* Host test: exercise the NetSurf media adapter with a fake decoder. */
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "pax_inline_media.c"

struct pax_inline_player { int playing; };
static struct pax_inline_player fake;
static uint16_t frame[] = {0xf800,0x07e0,0x001f,0xffff};
static unsigned opens, closes;
pax_inline_player_t *pax_inline_open(const char *u) { assert(u && *u); ++opens; return &fake; }
void pax_inline_close(pax_inline_player_t *p) { assert(p==&fake); ++closes; }
void pax_inline_play(pax_inline_player_t *p) { p->playing=1; }
void pax_inline_pause(pax_inline_player_t *p) { p->playing=0; }
void pax_inline_seek(pax_inline_player_t *p,double s) { (void)p;(void)s; }
void pax_inline_volume(pax_inline_player_t *p,float v,bool muted) { (void)p;(void)v;(void)muted; }
void pax_inline_loop(pax_inline_player_t *p,bool b) { (void)p;(void)b; }
void pax_inline_update(pax_inline_player_t *p,pax_inline_status_t *out)
{ memset(out,0,sizeof(*out));out->state=p->playing?PAX_INLINE_PLAYING:PAX_INLINE_PAUSED;out->frame_seq=1; }
const uint16_t *pax_inline_frame(pax_inline_player_t *p,int *w,int *h)
{ (void)p;*w=2;*h=2;return frame; }
static uint8_t canvas[4*10];
nsfb_t *framebuffer_current_surface(void) { return (nsfb_t *)(uintptr_t)1; }
int nsfb_get_geometry(nsfb_t *n,int *w,int *h,enum nsfb_format_e *fmt)
{ (void)n;*w=4;*h=4;*fmt=NSFB_FMT_RGB565;return 0; }
int nsfb_get_buffer(nsfb_t *n,uint8_t **p,int *stride)
{ (void)n;*p=canvas;*stride=10;return 0; }
bool nsfb_plot_get_clip(nsfb_t *n,nsfb_bbox_t *b)
{ (void)n;b->x0=1;b->y0=0;b->x1=4;b->y1=4;return true; }
static uint16_t pixel(int x,int y) { uint16_t v;memcpy(&v,canvas+y*10+x*2,2);return v; }
int main(void)
{
 struct gui_media *a=media_create("file:///sd/a.mp4",NULL),*b=media_create("file:///sd/b.mp3",NULL);
 assert(a && b && opens==0);
 struct gui_media_status s;media_status(a,&s);assert(s.state==GUI_MEDIA_READY);
 media_play(a);assert(opens==1);media_status(a,&s);assert(s.state==GUI_MEDIA_PLAYING);
 media_play(b);media_status(b,&s);assert(s.state==GUI_MEDIA_ERROR);
 struct rect dst={0,0,4,4}, clip={0,1,3,4};
 assert(media_redraw(a,NULL,&dst,&clip));
 assert(pixel(0,1)==0 && pixel(1,1)==0xf800 && pixel(2,1)==0x07e0);
 assert(pixel(1,2)==0x001f && pixel(2,2)==0xffff && pixel(3,2)==0);
 assert(canvas[8]==0 && canvas[9]==0);
 memset(canvas,0,sizeof(canvas));
 struct rect native={1,1,3,3}, full={0,0,4,4};
 assert(media_redraw(a,NULL,&native,&full));
 assert(pixel(1,1)==0xf800 && pixel(2,1)==0x07e0);
 assert(pixel(1,2)==0x001f && pixel(2,2)==0xffff && pixel(3,2)==0);
 struct rect empty={3,3,4,4};
 assert(media_redraw(a,NULL,&native,&empty));
 memset(canvas,0,sizeof(canvas));
 struct rect uneven={0,0,3,3};
 assert(media_redraw(a,NULL,&uneven,&full));
 assert(pixel(1,0)==0xf800 && pixel(2,0)==0x07e0);
 assert(pixel(1,2)==0x001f && pixel(2,2)==0xffff);
 media_pause(a);media_status(a,&s);assert(s.state==GUI_MEDIA_PAUSED);
 media_play(b);assert(opens==2 && closes==1);media_status(b,&s);assert(s.state==GUI_MEDIA_PLAYING);
 media_destroy(a);media_destroy(b);assert(closes==2);
 return 0;
}
