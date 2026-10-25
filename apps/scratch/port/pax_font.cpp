#include "pax_port.h"
#include <filesystem.hpp>
#include <os.hpp>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <map>
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>
namespace {
std::vector<unsigned char> fontData;
stbtt_fontinfo font;
bool attempted;
struct Glyph { int w=0,h=0,x=0,y=0; float advance=0; std::vector<unsigned char> alpha; };
struct Face { float scale; int baseline,height; std::map<unsigned char,Glyph> glyphs; };
std::map<int,Face> faces;
bool loadFont() {
 if (attempted) return !fontData.empty();
 attempted=true;
 auto path=OS::getRomFSLocation()+"gfx/ingame/fonts/NotoSans-Medium.ttf";
 FILE*f=fopen(path.c_str(),"rb");if(!f){pax_svc->log_printf("Scratch: UI font unavailable; using pixel fallback\n");return false;}
 fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);
 if(size<=0||size>2*1024*1024){fclose(f);return false;}
 fontData.resize(size);bool ok=fread(fontData.data(),1,size,f)==(size_t)size;fclose(f);
 int offset=ok?stbtt_GetFontOffsetForIndex(fontData.data(),0):-1;
 if(offset<0||!stbtt_InitFont(&font,fontData.data(),offset)){fontData.clear();return false;}
 pax_svc->log_printf("Scratch: antialiased Noto Sans UI font loaded (%u bytes)\n",(unsigned)fontData.size());
 return true;
}
Face& face(float scale) {
 int height=std::clamp((int)std::lround(24*scale),6,96);
 auto it=faces.find(height);
 if(it==faces.end()) {
  if(faces.size()>=8)faces.clear();
  Face f{};f.height=height;f.scale=stbtt_ScaleForPixelHeight(&font,height);
  int ascent,descent,gap;stbtt_GetFontVMetrics(&font,&ascent,&descent,&gap);
  // Match the top-origin UI convention: a small cap-height padding,
  // instead of the font ascent's larger reserved accent/line spacing.
  int x0,y0,x1,y1;stbtt_GetCodepointBitmapBox(&font,'H',f.scale,f.scale,&x0,&y0,&x1,&y1);
  f.baseline=-y0+std::max(1,(int)std::lround(height*.1f));
  it=faces.emplace(height,std::move(f)).first;
 }
 return it->second;
}
Glyph& glyph(Face& f,unsigned char c) {
 auto it=f.glyphs.find(c);if(it!=f.glyphs.end())return it->second;
 Glyph g;int advance,bearing;stbtt_GetCodepointHMetrics(&font,c,&advance,&bearing);g.advance=advance*f.scale;
 int x1,y1;stbtt_GetCodepointBitmapBox(&font,c,f.scale,f.scale,&g.x,&g.y,&x1,&y1);
 g.w=x1-g.x;g.h=y1-g.y;g.alpha.resize(g.w*g.h);
 if(g.w&&g.h)stbtt_MakeCodepointBitmap(&font,g.alpha.data(),g.w,g.h,g.w,f.scale,f.scale,c);
 return f.glyphs.emplace(c,std::move(g)).first->second;
}
}
bool pax_font_measure(const std::string& text,float scale,float& width,float& height) {
 if(scale<=0||!loadFont())return false;
 auto&f=face(scale);float x=0;int lines=1;width=0;
 for(unsigned char c:text){if(c=='\n'){width=std::max(width,x);x=0;++lines;}else x+=glyph(f,c).advance;}
 width=std::max(width,x);height=lines*f.height;return true;
}
bool pax_font_draw(const std::string& text,int x,int y,int color,float scale,bool centered) {
 float width,height;if(!pax_font_measure(text,scale,width,height))return false;
 auto&f=face(scale);float pen=0;int top=y;
 int origin=centered?x-(int)std::lround(width/2):x;
 int r=(color>>24)&255,g=(color>>16)&255,b=(color>>8)&255,a=color&255;
 for(unsigned char c:text){
  if(c=='\n'){pen=0;top+=f.height;continue;}
  auto&gl=glyph(f,c);int left=origin+(int)std::lround(pen)+gl.x;
  for(int yy=0;yy<gl.h;++yy)for(int xx=0;xx<gl.w;++xx){int alpha=gl.alpha[yy*gl.w+xx];if(alpha)pax_blend(left+xx,top+f.baseline+gl.y+yy,r,g,b,a*alpha/255);}
  pen+=gl.advance;
 }
 return true;
}

int pax_text_center_y(const std::string& text,float scale,float center) {
 if(!loadFont())return (int)std::lround(center-12*scale);
 auto&f=face(scale);int top=100000,bottom=-100000,line=0;
 for(unsigned char c:text){
  if(c=='\n'){line+=f.height;continue;}
  if(c<32||c>126)c='?';
  auto&g=glyph(f,c);if(!g.h)continue;
  top=std::min(top,line+f.baseline+g.y);bottom=std::max(bottom,line+f.baseline+g.y+g.h-1);
 }
 if(bottom<top)return (int)std::lround(center-f.height/2.f);
 return (int)std::lround(center-(top+bottom)/2.f);
}
