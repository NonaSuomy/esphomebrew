/* Compile against the Switch fbfx.c after applying switch-rgb565.patch. */
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "fbfx.c"

static uint8_t pixels[4 * 10]; /* four rows of four pixels plus padding */
nsfb_t *framebuffer_current_surface(void) { return (nsfb_t *)(uintptr_t)1; }
int nsfb_get_geometry(nsfb_t *n, int *w, int *h, enum nsfb_format_e *f)
{ (void)n; *w = 4; *h = 4; *f = NSFB_FMT_RGB565; return 0; }
int nsfb_get_buffer(nsfb_t *n, uint8_t **p, int *stride)
{ (void)n; *p = pixels; *stride = 10; return 0; }
bool nsfb_plot_get_clip(nsfb_t *n, nsfb_bbox_t *clip)
{ (void)n; clip->x0 = 0; clip->y0 = 0; clip->x1 = 4; clip->y1 = 4; return true; }
static uint16_t get_pixel(int x, int y)
{ uint16_t p; memcpy(&p, pixels + y * 10 + x * 2, sizeof(p)); return p; }
int main(void)
{
 struct fx_surface s = {.ptr=pixels,.stride=10,.bpp=2};
 fx_blend(&s,pixels,255,0,0,256); assert(get_pixel(0,0)==0xf800);
 fx_blend(&s,pixels+2,0,255,0,256); assert(get_pixel(1,0)==0x07e0);
 fx_blend(&s,pixels+4,0,0,255,256); assert(get_pixel(2,0)==0x001f);
 fx_blend(&s,pixels+10,255,255,255,128); assert(get_pixel(0,1)==0x7bef);
 assert(pixels[8]==0 && pixels[9]==0);
 memset(pixels, 0, sizeof(pixels));
 struct rect area={0,0,4,4};
 struct plot_gradient grad={.dx=1,.length=4,.nstops=2,.stop_colour={0x000000ff,0x00ff0000},.stop_pos={0,1}};
 assert(fbfx_gradient(NULL,&area,&grad)==NSERROR_OK);
 assert((get_pixel(0,0)&0xf800) > (get_pixel(3,0)&0xf800));
 assert((get_pixel(0,0)&0x1f) < (get_pixel(3,0)&0x1f));
 uint8_t saved[sizeof(pixels)]; memcpy(saved,pixels,sizeof(pixels));
 assert(fbfx_layer_begin(NULL,&area)==NSERROR_OK);
 memset(pixels,0xff,sizeof(pixels));
 struct plot_layer layer={.box={0,0,4,4},.opacity=0,.brightness=1};
 assert(fbfx_layer_end(NULL,&layer)==NSERROR_OK);
 for (int y=0;y<4;y++) assert(memcmp(saved+y*10,pixels+y*10,8)==0);
 const uint32_t red_video[1] = { 0x000000ffu };
 struct rect one={0,0,1,1};
 fbfx_blit_rgbx(NULL,red_video,1,1,&one,NULL);
 assert(get_pixel(0,0)==0xf800);
 s.bpp=4; s.bgr=0; fx_blend(&s,pixels,255,0,0,256); assert(fx_read(&s,pixels)==0xffff0000u);
 return 0;
}
