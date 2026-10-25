#define PAX_APP_SIDE 1
#include "pax_platform.h"
#include "pax_menu.h"
#include "pax_savegame.h"
#include "pax_pixels.h"
#include "pax_results.h"
#include <zlib.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>

PaxResults pax_results{};

extern "C" const unsigned char _binary_stargus_menu_bin_start[] __attribute__((weak));
extern "C" const unsigned char _binary_stargus_menu_bin_end[] __attribute__((weak));

namespace {
char chosenSaveName[64];
bool gameIsTutorial;
unsigned le16(const Uint8 *p) { return p[0] | unsigned(p[1])<<8; }
unsigned le32(const Uint8 *p) { return le16(p) | le16(p+2)<<16; }
bool readLine(FILE *file,char *line,size_t capacity) {
	// PAX FILE handles belong to the service, not this image's newlib FILE
	// layout. fgetc is bridged and buffered; calling libc fgets is unsafe.
	size_t used=0; int c;
	while ((c=fgetc(file))!=EOF && c!='\n') if (used+1<capacity) line[used++]=char(c);
	line[used]=0; return used || c!=EOF;
}
struct Media {
	const Uint8 *data{}; unsigned bytes{}, width{}, height{}, frames{}, interval{};
	unsigned fileOffset{}; bool fromFile{};
	bool independent{};
	Uint8 **cached{}; unsigned cacheCount{};
	FILE *stream{}; Uint8 *compressed{}; unsigned compressedCapacity{};
	Uint8 *decoded{}; unsigned capacity{}, current{~0u}, colors{}, kind{};
	Uint16 palette[256]{}; Uint8 alpha[256]{};
	bool open(const char *name) {
		const Uint8 *p=_binary_stargus_menu_bin_start, *end=_binary_stargus_menu_bin_end;
		if (!p || !end) {
			FILE *file=fopen(PAX_STRATAGUS_DATA_ROOT "/media/menu.paxmenu","rb");
			if (!file) return false;
			Uint8 header[12], entry[6]; char candidate[128]; bool found=false;
			if (fread(header,1,12,file)==12 && !memcmp(header,"PAPMENU1",8)) {
				const unsigned count=le32(header+8);
				for (unsigned i=0;i<count && i<64;++i) {
					if (fread(entry,1,6,file)!=6) break;
					const unsigned n=le16(entry),size=le32(entry+2);
					if (!n || n>=sizeof(candidate) || size>64*1024*1024) break;
					if (fread(candidate,1,n,file)!=n) break;
					if (strlen(name)==n && !memcmp(candidate,name,n)) {
						fileOffset=ftell(file); bytes=size; fromFile=true;
						if (fread(header,1,8,file)!=8) break;
						width=le16(header); height=le16(header+2); frames=le16(header+4); interval=le16(header+6);
						independent=interval&0x8000; interval&=0x7fff;
						found=width && width<=800 && height && height<=5000 && frames && frames<=8192 &&
							interval && 8+(frames+1)*4<=bytes; break;
					}
					if (fseek(file,size,SEEK_CUR)) break;
				}
			}
			fclose(file); return found;
		}
		if (end-p<12 || memcmp(p,"PAPMENU1",8)) return false;
		const unsigned entries=le32(p+8); p+=12;
		for (unsigned i=0;i<entries && end-p>=6;++i) {
			unsigned n=le16(p), size=le32(p+2); p+=6;
			if (n>unsigned(end-p) || size>unsigned(end-p-n)) return false;
			if (strlen(name)==n && !memcmp(p,name,n)) {
				data=p+n; bytes=size;
				if (size<12) return false;
				width=le16(data); height=le16(data+2); frames=le16(data+4); interval=le16(data+6);
				independent=interval&0x8000; interval&=0x7fff;
				if (!width || !height || width>800 || height>5000 || !frames || !interval ||
					frames>8192 || 8+(frames+1)*4>bytes) return false;
				return true;
			}
			p+=n+size;
		}
		return false;
	}
	bool decode(unsigned frame) {
		if (!frames) return false;
		if (!data && fromFile) {
			FILE *file=fopen(PAX_STRATAGUS_DATA_ROOT "/media/menu.paxmenu","rb");
			// The intro is larger than the launcher's largest free PSRAM
			// block. Keep only its index and stream compressed frames from SD.
			const bool streaming=bytes>256*1024;
			const unsigned size=streaming?8+(frames+1)*4:bytes;
			Uint8 *loaded=(Uint8 *)_pax_svc->mem_caps_alloc(size,PAX_MEM_CAP_SPIRAM);
			if (!file || !loaded || fseek(file,fileOffset,SEEK_SET) || fread(loaded,1,size,file)!=size) {
				if (file) fclose(file); if (loaded) _pax_svc->mem_free(loaded); return false;
			}
			if (streaming) stream=file; else fclose(file);
			data=loaded;
		}
		if (!data) return false;
		frame%=frames;
		if (current==frame) return true;
		const unsigned base=8+(frames+1)*4;
		// A keyframe every 15 frames bounds seeking and dropped-frame catchup.
		unsigned first=independent?frame:((current!=~0u && current<frame && frame-current<15)?current+1:frame/15*15);
		for (unsigned f=first;f<=frame;++f) {
			const unsigned start=le32(data+8+f*4), stop=le32(data+12+f*4);
			if (stop<=start || stop>bytes-base) { current=~0u; return false; }
			unsigned length=stop-start;
			const Uint8 *p;
			if (stream) {
				if (length>compressedCapacity) {
					if (compressed) _pax_svc->mem_free(compressed);
					compressed=(Uint8 *)_pax_svc->mem_caps_alloc(length,PAX_MEM_CAP_SPIRAM);
					compressedCapacity=compressed?length:0;
				}
				if (!compressed) { current=~0u; return false; }
				if (cached) {
					unsigned offset=start, copied=0;
					while (copied<length) {
						unsigned amount=65536-offset%65536;
						if (amount>length-copied) amount=length-copied;
						memcpy(compressed+copied,cached[offset/65536]+offset%65536,amount);
						offset+=amount; copied+=amount;
					}
				} else if (fseek(stream,fileOffset+base+start,SEEK_SET) ||
					fread(compressed,1,length,stream)!=length) { current=~0u; return false; }
				p=compressed;
			} else p=data+base+start;
			kind=*p++; --length;
			colors=kind==1 && length>=2 ? le16(p) : 0;
			if (kind<1 || kind>5) { current=~0u; return false; }
			if (kind==1) { if (!colors || colors>256 || length<3) return false; p+=2; length-=2; }
			const unsigned needed=width*height*(kind==1?1:2)+colors*4;
			// RGB animations need a second small buffer for XOR deltas; the
			// previous frame stays intact until inflate succeeds.
			const unsigned allocated=kind==1?needed:needed*2;
			if (allocated>capacity) {
				if (decoded) _pax_svc->mem_free(decoded);
				decoded=(Uint8 *)_pax_svc->mem_caps_alloc(allocated,PAX_MEM_CAP_SPIRAM);
				capacity=decoded ? allocated : 0;
			}
			if (!decoded) { current=~0u; return false; }
			const bool delta=kind==4 || kind==5;
			if (delta && current!=f-1) { current=~0u; return false; }
			uLongf inflated=needed;
			Uint8 *target=decoded+(delta?needed:0);
			if (uncompress(target,&inflated,p,length)!=Z_OK || inflated!=needed) { current=~0u; return false; }
			if (delta) {
				unsigned i=0; for (;i+4<=needed;i+=4) {
					Uint32 a,b; memcpy(&a,decoded+i,4); memcpy(&b,target+i,4); a^=b; memcpy(decoded+i,&a,4);
				}
				for (;i<needed;++i) decoded[i]^=target[i];
			}
			current=f;
		}
		for (unsigned i=0;i<colors;++i) {
			const Uint8 *c=decoded+i*4;
			palette[i]=pax_rgb_to_rgb565(unsigned(c[0])<<16 | unsigned(c[1])<<8 | c[2]); alpha[i]=c[3];
		}
		return true;
	}
	bool cacheMovie() {
		if (!decode(0)) return false;
		if (!stream || cached) return true;
		const unsigned base=8+(frames+1)*4, size=bytes-base;
		cacheCount=(size+65535)/65536;
		cached=(Uint8 **)_pax_svc->mem_caps_alloc(cacheCount*sizeof(*cached),PAX_MEM_CAP_SPIRAM);
		if (!cached) { cacheCount=0; return true; }
		memset(cached,0,cacheCount*sizeof(*cached));
		bool okay=fseek(stream,fileOffset+base,SEEK_SET)==0;
		for (unsigned i=0;okay && i<cacheCount;++i) {
			const unsigned amount=size-i*65536>65536?65536:size-i*65536;
			cached[i]=(Uint8 *)_pax_svc->mem_caps_alloc(amount,PAX_MEM_CAP_SPIRAM);
			okay=cached[i] && fread(cached[i],1,amount,stream)==amount;
		}
		if (!okay) {
			for (unsigned i=0;i<cacheCount;++i) if (cached[i]) _pax_svc->mem_free(cached[i]);
			_pax_svc->mem_free(cached); cached=nullptr; cacheCount=0;
		}
		if (_pax_svc->log_printf) _pax_svc->log_printf("Stargus: intro compressed cache=%u bytes blocks=%u\n",cached?size:0,cacheCount);
		return true; // A cache allocation failure falls back to streamed frames.
	}
	void close() {
		if (cached) {
			for (unsigned i=0;i<cacheCount;++i) if (cached[i]) _pax_svc->mem_free(cached[i]);
			_pax_svc->mem_free(cached); cached=nullptr; cacheCount=0;
		}
		if (decoded) _pax_svc->mem_free(decoded);
		if (stream) fclose(stream);
		if (compressed) _pax_svc->mem_free(compressed);
		stream=nullptr; compressed=nullptr; compressedCapacity=0;
		if (fromFile && data) { _pax_svc->mem_free((void *)data); data=nullptr; }
		decoded=nullptr; capacity=0; current=~0u;
	}
	void draw(SDL_Surface *screen,int x,int y,unsigned frame=0,int w=0,int h=0,bool preserveEdges=false) {
		if (!decode(frame)) return;
		if (!w) w=width; if (!h) h=height;
		const int left=x<0?-x:0, top=y<0?-y:0;
		const int right=x+w>screen->w?screen->w-x:w, bottom=y+h>screen->h?screen->h-y:h;
		if (right<=left || bottom<=top) return;
		const Uint8 *indices=decoded+colors*4;
		// Division inside every scaled pixel cost precious intro frame time.
		// Keep clipped source columns on the stack, bounded by our 800px canvas.
		Uint16 sourceX[800];
		// Small menu buttons have a one-pixel outer border. Ordinary nearest
		// scaling can omit the final source column when shrinking them.
		for (int xx=left;xx<right;++xx) sourceX[xx-left]=preserveEdges && w>1?
			unsigned(xx)*(width-1)/(w-1):unsigned(xx)*width/w;
		const bool opaque=kind==2 || kind==5;
		unsigned previousY=~0u;
		for (int yy=top;yy<bottom;++yy) {
			const unsigned sy=preserveEdges && h>1?unsigned(yy)*(height-1)/(h-1):unsigned(yy)*height/h;
			Uint16 *dst=(Uint16 *)((Uint8 *)screen->pixels+(y+yy)*screen->pitch)+x+left;
			if (opaque && w==int(width)) {
				memcpy(dst,decoded+(sy*width+left)*2,(right-left)*2);
				continue;
			}
			if (opaque && sy==previousY) {
				memcpy(dst,(Uint8 *)dst-screen->pitch,(right-left)*2);
				continue;
			}
			previousY=sy;
			for (int xx=left;xx<right;++xx) {
				const unsigned at=sy*width+sourceX[xx-left];
				if (kind!=1) { const Uint16 pixel=le16(decoded+at*2); if (pixel || kind==2 || kind==5) dst[xx-left]=pixel; }
				else { unsigned i=indices[at]; if (i<colors && alpha[i]) dst[xx-left]=palette[i]; }
			}
		}
	}
	unsigned frame(Uint32 elapsed) const { return interval && frames ? (elapsed/interval)%frames : 0; }
};

struct Button { SDL_Rect rect; const char *label; SDL_Keycode hotkey; };
bool inside(const SDL_Rect &r,int x,int y) { return x>=r.x && y>=r.y && x<r.x+r.w && y<r.y+r.h; }
enum Page { Main, Factions, Missions, Message, Options };
struct Menu {
	SDL_Surface *screen;
	Media background,title,font,button,cursor,intro;
	Media idle[3],on[3],race[3],raceOn[3],disk;
	Page page{Main},previous{Main}; int selected{-1},faction{1},pressed{-1};
	Uint32 began{},hoverBegan{}; const char *message{};
	Button buttons[16]{}; int count{};
	int widths[224]{};
	int fontCellW{25},fontCellH{19};
	unsigned factionLast[3]{~0u,~0u,~0u},factionShown[3]{},factionDropped[3]{},factionWorst{};
	bool init() {
		bool okay=background.open("graphics/ui/Menu background without title.png") &&
			title.open("graphics/ui/title.png") && font.open("graphics/ui/fonts/font16x.png") &&
			button.open("graphics/ui/readyt/butterr.png") && cursor.open("graphics/ui/cursors/arrow.png");
		okay &= intro.open("videos/blizzard.ogv") && disk.open("videos/campaign/disk.mng");
		const char *names[]={"single","multi","exit"},*races[]={"zerg","terr","prot"};
		for (int i=0;i<3;++i) {
			char path[96]; snprintf(path,sizeof(path),"videos/mainmenu/%s.mng",names[i]); okay &= idle[i].open(path);
			snprintf(path,sizeof(path),"videos/mainmenu/%son.mng",names[i]); okay &= on[i].open(path);
			snprintf(path,sizeof(path),"videos/campaign/%s.mng",races[i]); okay &= race[i].open(path);
			snprintf(path,sizeof(path),"videos/campaign/%son.mng",races[i]); okay &= raceOn[i].open(path);
		}
		if (!okay || !initFont()) return false;
		began=SDL_GetTicks(); return true;
	}
	bool initFont() {
		if (!font.decode(0)) return false;
		const Uint8 *pixels=font.decoded+font.colors*4;
		const int columns=font.width/fontCellW, glyphs=(font.height/fontCellH)*columns;
		memset(widths,0,sizeof(widths)); widths[0]=fontCellW/2;
		for (int c=1;c<224 && c<glyphs;++c) {
			for (int y=0;y<fontCellH;++y) for (int x=0;x<fontCellW;++x) {
				const int at=((c/columns)*fontCellH+y)*font.width+(c%columns)*fontCellW+x;
				const unsigned index=pixels[at];
				if (index<font.colors && font.alpha[index] && font.palette[index]!=0)
					if (x+1>widths[c]) widths[c]=x+1;
			}
		}
		return true;
	}
	void close() {
		background.close(); title.close(); font.close(); button.close(); cursor.close(); intro.close(); disk.close();
		for (int i=0;i<3;++i) { idle[i].close(); on[i].close(); race[i].close(); raceOn[i].close(); }
	}
	int textWidth(const char *text) const {
		int w=0; for (;*text;++text) { unsigned c=(Uint8)*text; if (c>=32) w+=widths[c-32]+1; } return w;
	}
	void text(const char *caption,int x,int y,Uint16 color,bool greenInitial=false,bool center=false,int greenAt=-1,int scale=100) {
		if (center) x-=textWidth(caption)*scale/200;
		const Uint8 *pixels=font.decoded+font.colors*4;
		const int columns=font.width/fontCellW;
		bool first=true; int character=0;
		for (;*caption;++caption) {
			const unsigned ch=(Uint8)*caption; if (ch<32) continue;
			const unsigned c=ch-32;
			const Uint16 ink=(first && greenInitial || character==greenAt)?0x07e0:color;
			for (int yy=0;yy<fontCellH*scale/100;++yy) for (int xx=0;xx<widths[c]*scale/100;++xx) {
				const int px=x+xx,py=y+yy;
				if (px<0 || py<0 || px>=screen->w || py>=screen->h) continue;
				const unsigned at=((c/columns)*fontCellH+yy*100/scale)*font.width+(c%columns)*fontCellW+xx*100/scale;
				if (at>=font.width*font.height) continue;
				const unsigned index=pixels[at];
				if (index<font.colors && font.alpha[index] && font.palette[index])
					((Uint16 *)((Uint8 *)screen->pixels+py*screen->pitch))[px]=ink;
			}
			x+=(widths[c]+1)*scale/100; first=false; ++character;
		}
	}
	void add(int x,int y,int w,int h,const char *label,SDL_Keycode key) {
		if (count<16) buttons[count++]={{x,y,w,h},label,key};
	}
	void layout() {
		count=0;
		if (page==Main) {
			add(100,50,320,116,"Single Player",SDLK_s);
			add(80,250,idle[1].width,idle[1].height,"Multiplayer",SDLK_m);
			add(500,280,idle[2].width,idle[2].height,"Exit Program",SDLK_x);
			add(100,420,180,19,"Options",SDLK_o);
			add(100,440,180,19,"Show Info",SDLK_i);
			add(100,460,180,19,"Show Credits",SDLK_d);
		} else if (page==Factions) {
			add(133-int(race[0].width)/2+5,85,race[0].width,race[0].height,"Zerg",SDLK_z);
			add(365-int(race[1].width)/2,50,race[1].width,race[1].height,"Terran",SDLK_t);
			add(667-int(race[2].width)/2,100,race[2].width,race[2].height,"Protoss",SDLK_p);
			add(314,342,172,35,"Skirmish",SDLK_s); add(314,385,172,35,"Load",SDLK_l);
			add(314,428,172,35,"Replay",SDLK_r); add(581,402,172,35,"Cancel",SDLK_c);
		} else if (page==Missions) {
			const int missions=faction==1?13:11;
			for (int i=0;i<missions;++i) add(300,16+i*29,200,25,"",SDLK_UNKNOWN);
			add(314,428,172,35,"Cancel",SDLK_c);
		} else if (page==Options) {
			add(314,190,172,30,"Replay Intro",SDLK_r);
			add(314,380,172,35,"Cancel",SDLK_c);
		} else add(314,380,172,35,"Back",SDLK_c);
	}
	int hit(int x,int y) const { for (int i=0;i<count;++i) if (inside(buttons[i].rect,x,y)) return i; return -1; }
	void change(Page next) {
		if (page==Factions && next!=Factions) {
			if (_pax_svc->log_printf) _pax_svc->log_printf("Stargus: faction animation presented=%u/%u/%u dropped=%u/%u/%u worst=%u ms\n",
				factionShown[0],factionShown[1],factionShown[2],factionDropped[0],factionDropped[1],factionDropped[2],factionWorst);
			for (int i=0;i<3;++i) { race[i].close(); raceOn[i].close(); }
			disk.close();
		}
		if (next==Factions && page!=Factions) {
			for (int i=0;i<3;++i) { race[i].cacheMovie(); raceOn[i].cacheMovie(); }
			disk.cacheMovie();
			began=SDL_GetTicks();
			for (int i=0;i<3;++i) { factionLast[i]=~0u; factionShown[i]=factionDropped[i]=0; } factionWorst=0;
		}
		page=next; selected=pressed=-1; hoverBegan=SDL_GetTicks(); layout();
		if (_pax_svc->log_printf) _pax_svc->log_printf("Stargus: original menu page=%d\n",int(page));
	}
	void notice(const char *what) { previous=page; message=what; change(Message); }
	bool cinematic(Media &media,unsigned duration,bool movie) {
		if (!media.frames) return true;
		const Uint32 start=SDL_GetTicks();
		unsigned shown=~0u, presented=0, dropped=0, decodeMs=0, drawMs=0, presentMs=0, worstMs=0;
		SDL_FillRect(screen,nullptr,0);
		if (_pax_svc->log_printf) _pax_svc->log_printf("Stargus: intro stage=%s frames=%u\n",movie?"Blizzard video":"three-faction title",media.frames);
		while (!pax_exit_requested && SDL_GetTicks()-start<duration) {
			SDL_Event event{}; bool skip=false;
			for (int i=0;i<64 && SDL_PollEvent(&event);++i) {
				if (event.type==SDL_QUIT) pax_exit_requested=1;
				if (event.type==SDL_KEYDOWN || event.type==SDL_MOUSEBUTTONUP) skip=true;
			}
			pax_stratagus_input_poll();
			if (skip) break;
			const unsigned frame=movie ? (SDL_GetTicks()-start)/media.interval : 0;
			if (frame>=media.frames) break;
			if (frame==shown) { SDL_Delay(2); continue; }
			const Uint32 began=SDL_GetTicks();
			if (!media.decode(frame)) {
				if (_pax_svc->log_printf) _pax_svc->log_printf("Stargus: intro frame decode failed\n");
				media.close(); return false;
			}
			const Uint32 decodedAt=SDL_GetTicks();
			// Keep the movie's aspect ratio, and the original 640x480 title art.
			const int h=480, w=movie?int(media.width*480/media.height):640;
			media.draw(screen,(800-w)/2,0,frame,w,h);
			const Uint32 drawnAt=SDL_GetTicks();
			pax_stratagus_video_present565(screen->pixels,800,480);
			const Uint32 done=SDL_GetTicks();
			decodeMs+=decodedAt-began; drawMs+=drawnAt-decodedAt; presentMs+=done-drawnAt;
			if (done-began>worstMs) worstMs=done-began;
			dropped+=shown==~0u?frame:frame-shown-1;
			shown=frame; ++presented;
			// The wall clock chooses the next frame; display time is part of
			// its budget, not an extra fixed delay after every presentation.
		}
		if (movie && presented && _pax_svc->log_printf)
			_pax_svc->log_printf("Stargus: intro playback interval=%u presented=%u dropped=%u avg-decode=%u avg-draw=%u avg-present=%u worst=%u ms\n",
				media.interval,presented,dropped,decodeMs/presented,drawMs/presented,presentMs/presented,worstMs);
		media.close(); return !pax_exit_requested;
	}
	bool titles() {
		// SD reads happen before starting the soundtrack/playback clock.
		// Small blocks tolerate a fragmented PSRAM heap; failure streams instead.
		if (!intro.cacheMovie()) return false;
		pax_stratagus_audio_music(1); pax_stratagus_audio_start();
		if (!cinematic(intro,intro.frames*intro.interval,true)) return false;
		pax_stratagus_audio_music(2);
		return cinematic(title,5000,false);
	}
	int activate(int choice) {
		pax_stratagus_audio_named("button",0,0,255,2);
		if (choice<0 || choice>=count) return -1;
		if (_pax_svc->log_printf) _pax_svc->log_printf("Stargus: menu activate page=%d item=%d\n",int(page),choice);
		if (page==Main) {
			if (choice==0) change(Factions);
			else if (choice==2) return 0;
			else if (choice==3) change(Options);
			else notice(choice==1?"Multiplayer is not available in this PAX.":choice==4?
				"Stargus 3.3.0 / Stratagus ESP32-P4 port":"Stargus and Stratagus by the Wargus team.");
		} else if (page==Factions) {
			if (choice<3) { faction=choice; change(Missions); }
			else if (choice==6) change(Main);
			else if (choice==4) {
				const int action=pax_stratagus_game_menu_run(screen,2);
				if (action==50) {
					if (pax_stratagus_load_named(chosenSaveName)==0) return 2;
					notice("Saved game is incomplete or corrupt.");
				}
				if (action>=20 && action<23) {
					if (pax_stratagus_request_load_slot(action-20)==0) return 2;
					notice("Saved game is incomplete or corrupt.");
				}
			} else notice("Skirmish and replays are not ported yet.");
		} else if (page==Missions) {
			if (choice==count-1) change(Factions);
			else if (faction==1 && choice==0) return 3;
			else if (faction==1 && choice==1) return 1;
			else notice("This build supports Terran Missions 1 and 2.");
		} else if (page==Options) {
			if (choice==0) { titles(); change(Main); }
			else change(Main);
		} else change(previous);
		return -1;
	}
	void back() { if (page==Main) pax_exit_requested=1; else if (page==Factions || page==Options) change(Main);
		else if (page==Missions) change(Factions); else change(previous); }
	void ornament(int i) {
		const Button &b=buttons[i]; button.draw(screen,b.rect.x,b.rect.y-16);
		text(b.label,b.rect.x+b.rect.w/2,b.rect.y+3,selected==i?0xffff:0xffe0,false,true);
	}
	void draw() {
		SDL_FillRect(screen,nullptr,0); const Uint32 now=SDL_GetTicks(),elapsed=now-began;
		if (page==Main) {
			background.draw(screen,0,0,0,800,480);
			for (int i=0;i<3;++i) {
				const int xs[]={143,99,515},ys[]={115,261,280};
				// Single-player hover grid is behind the ships; the other
				// overlays are in front, matching addAnimatedButton in Lua.
				if (i==0 && selected==i) on[i].draw(screen,xs[i],ys[i],on[i].frame(now-hoverBegan));
				idle[i].draw(screen,buttons[i].rect.x,buttons[i].rect.y,idle[i].frame(elapsed));
				if (i!=0 && selected==i) on[i].draw(screen,xs[i],ys[i],on[i].frame(now-hoverBegan));
				const auto &r=buttons[i].rect;
				text(buttons[i].label,r.x+r.w-textWidth(buttons[i].label),r.y+r.h-19,0xffff,true);
			}
			for (int i=3;i<count;++i) text(buttons[i].label,buttons[i].rect.x,buttons[i].rect.y,
				selected==i?0xffe0:0xffff,i==3,false,i==4?5:i==5?8:-1);
		} else if (page==Factions) {
			const int centers[]={133,400,667},ys[]={245,210,260};
			for (int i=0;i<3;++i) {
				const unsigned tick=race[i].interval?elapsed/race[i].interval:0;
				if (tick!=factionLast[i]) {
					factionDropped[i]+=factionLast[i]==~0u?tick:tick-factionLast[i]-1;
					factionLast[i]=tick; ++factionShown[i];
				}
				disk.draw(screen,centers[i]-int(disk.width)/2,ys[i],disk.frame(elapsed));
				race[i].draw(screen,buttons[i].rect.x,buttons[i].rect.y,race[i].frame(elapsed));
				if (selected==i) { const int x[]={28,260,527},y[]={50,0,10};
					raceOn[i].draw(screen,x[i],y[i],raceOn[i].frame(now-hoverBegan)); }
				const auto &r=buttons[i].rect;
				text(buttons[i].label,r.x+r.w-textWidth(buttons[i].label),r.y+r.h-19,0xffff,true);
			}
			for (int i=3;i<count;++i) ornament(i);
		} else if (page==Missions) {
			for (int i=0;i<count-1;++i) { char caption[32]; snprintf(caption,sizeof(caption),"Mission %d",i+1);
				text(caption,400,buttons[i].rect.y,selected==i?0xffff:0xffe0,false,true); }
			ornament(count-1);
		} else {
			text(page==Options?"Options":"Stargus",400,100,0xffe0,false,true);
			if (message && page==Message) text(message,400,210,0xffff,false,true);
			for (int i=0;i<count;++i) ornament(i);
		}
		int x,y; SDL_GetMouseState(&x,&y); cursor.draw(screen,x,y);
		pax_stratagus_video_present565(screen->pixels,800,480);
		if (page==Factions) factionWorst=std::max(factionWorst,unsigned(SDL_GetTicks()-now));
	}
	int run(bool showIntro=true) {
		pax_stratagus_pointer_start();
		if (showIntro && !titles()) return 0;
		if (!showIntro) { pax_stratagus_audio_music(2); pax_stratagus_audio_start(); }
		change(Main);
#ifdef STRATAGUS_PAX_INPUT_SMOKE_TEST
		Uint32 smokeTime=SDL_GetTicks();
#endif
		while (!pax_exit_requested) {
#ifdef STRATAGUS_PAX_INPUT_SMOKE_TEST
			if (SDL_GetTicks()-smokeTime>=4000) {
				// Exercise the actual menu transitions, not a separate map shortcut.
			const int result=activate(page==Factions?1:page==Missions?STRATAGUS_PAX_TEST_MISSION-1:0);
				if (result>=0) return result;
				smokeTime=SDL_GetTicks();
			}
#endif
			SDL_Event event{};
			for (int i=0;i<64 && SDL_PollEvent(&event);++i) {
				int choose=-1;
				if (event.type==SDL_QUIT) return 0;
				if (event.type==SDL_MOUSEMOTION) {
					const int hover=hit(event.motion.x,event.motion.y);
					if (hover!=selected) { selected=hover; hoverBegan=SDL_GetTicks(); }
				} else if (event.type==SDL_MOUSEBUTTONDOWN && event.button.button==SDL_BUTTON_LEFT)
					pressed=hit(event.button.x,event.button.y);
				else if (event.type==SDL_MOUSEBUTTONUP && event.button.button==SDL_BUTTON_LEFT) {
					const int target=hit(event.button.x,event.button.y); if (target==pressed) choose=target; pressed=-1;
				} else if (event.type==SDL_KEYDOWN) {
					const auto key=event.key.keysym.sym;
					if (key==SDLK_ESCAPE || key==SDLK_x && page!=Main) back();
					else if (key==SDLK_UP || key==SDLK_LEFT || key==SDLK_DOWN || key==SDLK_RIGHT || key==SDLK_TAB) {
						const int step=key==SDLK_UP || key==SDLK_LEFT?-1:1;
						selected=selected<0?0:(selected+step+count)%count; hoverBegan=SDL_GetTicks();
					} else if (key==SDLK_RETURN || key==SDLK_SPACE || key==SDLK_z && page!=Factions) choose=selected<0?0:selected;
					else for (int n=0;n<count;++n) if (buttons[n].hotkey!=SDLK_UNKNOWN && buttons[n].hotkey==key) { choose=n; break; }
				}
				if (choose>=0) { const int result=activate(choose); if (result>=0) return result; }
			}
			pax_stratagus_input_poll(); draw(); SDL_Delay(10);
		}
		return 0;
	}
	void loading(int result) {
		SDL_FillRect(screen,nullptr,0);
		text(result==3?"\"BOOT CAMP\"":"TERRAN CAMPAIGN - WASTELAND",80,235,0xffe0);
		text(result==3?"Loading Mission 1...":result==1?"Loading Mission 2...":"Loading saved game...",80,320,0xffe0);
		pax_stratagus_video_present565(screen->pixels,800,480);
	}
	bool campaignText(int result) {
		if (result!=1 && result!=3) return true;
		char path[160]; snprintf(path,sizeof(path),"%s/campaigns/terran/%s",PAX_STRATAGUS_DATA_ROOT,result==3?"EstT0t.txt":"EstT01.txt");
		FILE *file=fopen(path,"rb"); if (!file) return false;
		// RunImageStep uses font16x, not the smaller in-game HUD font.
		font.close(); fontCellW=25; fontCellH=19;
		if (!font.open("graphics/ui/fonts/font16x.png") || !initFont()) { fclose(file); return false; }
		char line[1024],rows[48][160]{}; unsigned count=0,duration=5000,pageNumber=0;
		bool active=false,bottom=false;
		while (!pax_exit_requested && readLine(file,line,sizeof(line))) {
			line[strcspn(line,"\r\n")]=0;
			if (sscanf(line,"</DISPLAYTIME %u>",&duration)==1) continue;
			if (!strcmp(line,"</SCREENLEFT>") || !strcmp(line,"</SCREENLOWERLEFT>")) {
				active=true; bottom=!strcmp(line,"</SCREENLOWERLEFT>"); count=0; continue;
			}
			if (!strcmp(line,"</PAGE>")) {
				++pageNumber; active=false; SDL_FillRect(screen,nullptr,0);
				const int y=bottom?std::max(40,440-int(count)*20):40;
				for (unsigned i=0;i<count;++i) text(rows[i],80,y+i*20,0xffe0);
				pax_stratagus_video_present565(screen->pixels,800,480);
				if (_pax_svc->log_printf) _pax_svc->log_printf("Stargus: campaign text mission=%d page=%u rows=%u\n",result==3?1:2,pageNumber,count);
				const Uint32 began=SDL_GetTicks(); bool next=false;
				while (!pax_exit_requested && !next && SDL_GetTicks()-began<duration) {
					SDL_Event event{};
					while (SDL_PollEvent(&event)) {
						if (event.type==SDL_QUIT) pax_exit_requested=1;
						if (event.type==SDL_KEYDOWN || event.type==SDL_MOUSEBUTTONUP) next=true;
					}
					pax_stratagus_input_poll(); SDL_Delay(5);
				}
				continue;
			}
			if (!active || line[0]=='<' || count>=48) continue;
			if (!line[0]) { rows[count++][0]=0; continue; }
			char *word=strtok(line," "); unsigned used=0;
			while (word && count<48) {
				char candidate[160]; snprintf(candidate,sizeof(candidate),"%s%s%s",used?rows[count]:"",used?" ":"",word);
				if (used && textWidth(candidate)>640) { ++count; used=0; }
				if (count>=48) break;
				if (!used) snprintf(rows[count],160,"%s",word);
				else snprintf(rows[count]+used,160-used," %s",word);
				used=strlen(rows[count]); word=strtok(nullptr," ");
			}
			if (used && count<48) ++count;
		}
		fclose(file); return !pax_exit_requested;
	}
};

// The original scripts/menus/game.lua has seven entries in this order.
// Keep this modal separate from the startup menu and the game simulation.
struct GameSnapshot {
	static constexpr int Rows=40, Blocks=480/Rows;
	Uint16 *pixels[Blocks]{};
	bool capture(SDL_Surface *screen) {
		for (auto &block:pixels) {
			block=(Uint16 *)_pax_svc->mem_caps_alloc(800*Rows*2,PAX_MEM_CAP_SPIRAM);
			if (!block) { close(); return false; }
		}
		for (int y=0;y<480;++y)
			memcpy(pixels[y/Rows]+800*(y%Rows),(const Uint8 *)screen->pixels+y*screen->pitch,800*2);
		return true;
	}
	void restore(SDL_Surface *screen) const {
		// Backdrop is cosmetic. Boot Camp can leave less than the 768 KB
		// required for it; keep the controls usable on a black background.
		if (!pixels[0]) { SDL_FillRect(screen,nullptr,0); return; }
		for (int y=0;y<480;++y)
			memcpy((Uint8 *)screen->pixels+y*screen->pitch,pixels[y/Rows]+800*(y%Rows),800*2);
	}
	void close() { for (auto &block:pixels) { if (block) _pax_svc->mem_free(block); block=nullptr; } }
};
struct GameMenu {
	Menu art{}; Media panel{},browserPanel{},fullButton{},pressedButton{};
	int page{},selected{},pressed{-1};
	const char *notice{};
	char saveNames[32][64]{},filename[64]{"game.sav"};
	int saveCount{},firstSave{},visibleSaves{},saveIndex{-1}; bool editing{};
	bool init(SDL_Surface *screen) {
		art.screen=screen;
		return art.font.open("graphics/ui/fonts/font16x.png") && art.initFont() &&
			art.button.open("graphics/ui/readyt/butterr.png") &&
			art.cursor.open("graphics/ui/cursors/arrow.png") && panel.open("graphics/ui/panels/264x288.png") &&
			browserPanel.open("graphics/ui/panels/384x256.png") &&
			fullButton.open("graphics/ui/terran/agg/button_large.png") &&
			pressedButton.open("graphics/ui/terran/agg/button_large_pressed.png");
	}
	void close() { art.close(); panel.close(); browserPanel.close(); fullButton.close(); pressedButton.close(); }
	void refreshSaves() { saveCount=pax_stratagus_list_saves(saveNames,32); firstSave=0; saveIndex=-1; }
	void layout() {
		art.count=0;
		const bool small=page==1 || page==2;
		if (art.fontCellW!=(small?10:25)) {
			art.font.close(); art.fontCellW=small?10:25; art.fontCellH=small?13:19;
			if (!art.font.open(small?"graphics/ui/fonts/font10.png":"graphics/ui/fonts/font16x.png") || !art.initFont()) return;
		}
		if (!page) {
			const char *labels[]={"Save Game","Load Game","Options","Help","Mission Objectives","End Mission","Return to Game (Esc)"};
			const SDL_Keycode keys[]={SDLK_s,SDLK_l,SDLK_o,SDLK_h,SDLK_j,SDLK_e,SDLK_ESCAPE};
			for (int i=0;i<7;++i) art.add(196,102+i*42,408,36,labels[i],keys[i]);
		} else if (page==1 || page==2) {
			visibleSaves=std::min(6,saveCount-firstSave);
			if (page==1) art.add(192,112,416,28,filename,SDLK_UNKNOWN);
			for (int i=0;i<visibleSaves;++i) art.add(192,154+i*26,416,26,saveNames[firstSave+i],SDLK_UNKNOWN);
			art.add(180,365,128,32,page==1?"Save":"Load",page==1?SDLK_s:SDLK_l);
			art.add(336,365,128,32,page==1?"Delete":"Cancel",page==1?SDLK_d:SDLK_ESCAPE);
			if (page==1) art.add(492,365,128,32,"Cancel",SDLK_ESCAPE);
			if (saveCount>6) {
				art.add(192,318,188,26,"Previous",SDLK_LEFT);
				art.add(420,318,188,26,"Next",SDLK_RIGHT);
			}
		} else {
			if (page==6) {
				art.add(196,160,408,36,"Restart Mission",SDLK_r);
				art.add(196,208,408,36,"Quit Mission",SDLK_q);
				art.add(196,256,408,36,"Exit Program",SDLK_x);
			}
			if (page==7 || page==8) art.add(196,270,408,36,page==7?"Overwrite Save":"Load Saved Game",SDLK_RETURN);
			if (page>=9 && page<=11) art.add(196,270,408,36,page==9?"Restart Mission":page==10?"Quit Mission":"Exit Program",SDLK_RETURN);
			if (page==12) art.add(196,270,408,36,"Delete Save",SDLK_RETURN);
			art.add(196,350,408,35,page>=7?"Cancel (Esc)":"Previous (Esc)",SDLK_ESCAPE);
		}
	}
	int activate(int choice) {
		pax_stratagus_audio_named("button",0,0,255,2);
		notice=nullptr;
		if (!page) {
			if (choice==6) return 0; if (choice>=0 && choice<6) page=choice+1;
			if (page==1 || page==2) { refreshSaves(); editing=page==1; }
		} else if (page==1 || page==2) {
			const int base=page==1?1:0,action=base+visibleSaves;
			if (page==1 && choice==0) editing=true;
			else if (choice>=base && choice<action) {
				saveIndex=firstSave+choice-base; strcpy(filename,saveNames[saveIndex]); editing=false;
			} else if (choice==action) {
				strcpy(chosenSaveName,filename); editing=false;
				if (page==1) { if (!pax_stratagus_save_named_exists(filename)) return 40; page=7; }
				else if (saveIndex>=0) page=8; else notice="Select a saved game first.";
			} else if (choice==action+1 && page==1) {
				if (saveIndex>=0) { strcpy(chosenSaveName,filename); page=12; editing=false; }
				else notice="Select a saved game first.";
			} else if (choice==action+1 || choice==action+2) page=0;
			else if (saveCount>6) {
				const int previous=action+(page==1?3:2);
				firstSave=choice==previous?std::max(0,firstSave-6):std::min((saveCount-1)/6*6,firstSave+6);
			}
		}
		else if (page==7 && choice==0) return 40;
		else if (page==8 && choice==0) return 50;
		else if (page==12 && choice==0) {
			const bool okay=pax_stratagus_delete_named(chosenSaveName)==0;
			page=1; refreshSaves(); notice=okay?"Save deleted (including backup).":"Delete failed.";
		}
		else if (page==6 && choice>=0 && choice<3) page=9+choice;
		else if (page>=9 && page<=11 && choice==0) return page==9?2:page==10?1:3;
		else page=page==7 || page==12?1:page==8?2:page>=9 && page<=11?6:0;
		selected=0; pressed=-1; layout(); return -1;
	}
	void draw(const GameSnapshot &snapshot) {
		snapshot.restore(art.screen);
		// Original panel artwork, stretched vertically for readable touch targets.
		if (page==1 || page==2) browserPanel.draw(art.screen,144,70,0,512,340);
		else panel.draw(art.screen,176,52,0,448,386);
		const char *titles[]={"Game Menu","Save Game","Load Game","Options","Help","Mission Objectives","End Mission","Overwrite Save","Load Saved Game","Restart Mission","Quit Mission","Exit Program","Delete Save"};
		art.text(titles[page],400,page==1 || page==2?88:68,0xffe0,false,true);
		const char *lines[6]{};
		if (page==1 || page==2) {
			SDL_Rect list{192,154,416,156}; SDL_FillRect(art.screen,&list,0);
			if (!saveCount) art.text("No saved games.",208,164,0xffff);
			if (notice) art.text(notice,400,342,0xffff,false,true);
		}
		else if (page==3) { lines[0]="Audio/display settings:"; lines[1]="use the launcher's controls."; }
		else if (page==4) { lines[0]="Tap a unit to select."; lines[1]="Drag to select a group."; lines[2]="Tap ground: move, not attack."; lines[3]="Attack/A + target: fight."; lines[4]="Hold map: right-click order."; lines[5]="Mouse: left select, right order."; }
		else if (page==5 && gameIsTutorial) { lines[0]="Tutorial: Boot Camp"; lines[1]="Build 3 Supply Depots."; lines[2]="Build a Refinery."; lines[3]="Gather 100 Vespene Gas."; }
		else if (page==5) { lines[0]="Wasteland"; lines[1]="Find Raynor."; lines[2]="Build a Barracks."; lines[3]="Train 10 Marines."; }
		else if (page>=9 && page<=11) { lines[0]=page==9?"Restart this mission?":page==10?"Quit this mission?":"Exit Stargus?"; lines[1]="Unsaved progress will be lost."; }
		else if (page==7) { lines[0]="Replace the existing save?"; lines[1]="Previous save kept as backup."; }
		else if (page==8) { lines[0]="Load this saved game?"; lines[1]="Unsaved progress will be lost."; }
		else if (page==12) { lines[0]="Delete this saved game?"; lines[1]="Its backup will also be removed."; }
		for (int i=0;i<6;++i) if (lines[i]) art.text(lines[i],400,145+i*32,0xffff,false,true);
		for (int i=0;i<art.count;++i) {
			const auto &b=art.buttons[i];
			const int base=page==1?1:0;
			const bool row=(page==1 || page==2) && i>=base && i<base+visibleSaves;
			if (row || (page==1 && i==0)) {
				SDL_Rect rect=b.rect; SDL_FillRect(art.screen,&rect,row && firstSave+i-base==saveIndex?0x02a0:0);
				art.text(b.label,b.rect.x+8,b.rect.y+6,i==selected?0xffff:0xffe0);
				if (page==1 && i==0 && editing && (SDL_GetTicks()/500)%2==0)
					art.text("_",b.rect.x+8+art.textWidth(filename),b.rect.y+6,0xffff);
			} else {
				(pressed==i?pressedButton:fullButton).draw(art.screen,b.rect.x,b.rect.y,0,b.rect.w,b.rect.h,true);
				art.text(b.label,b.rect.x+b.rect.w/2,b.rect.y+8,i==selected?0xffff:0xffe0,false,true);
			}
		}
		int x,y; SDL_GetMouseState(&x,&y); art.cursor.draw(art.screen,x,y);
		pax_stratagus_video_present565(art.screen->pixels,800,480);
	}
};
Media portrait;
int portraitIndex{-1}; Uint32 portraitStart;
}

bool pax_stratagus_movie_play(SDL_Surface *screen,const char *name)
{
	if (!screen || !name || screen->format->BytesPerPixel!=2) return false;
	Media media;
	if (!media.open(name)) {
		if (_pax_svc->log_printf) _pax_svc->log_printf("Warcraft: video pack entry missing: %s\n",name);
		return false;
	}
	if (!media.decode(0)) { media.close(); return false; }
	char soundtrack[256];
	const char *base=strrchr(name,'/'); base=base ? base+1:name;
	snprintf(soundtrack,sizeof(soundtrack),"media/%s.ogg",base);
	pax_stratagus_audio_music_file(soundtrack);
	const Uint32 start=SDL_GetTicks(); unsigned shown=~0u,presented=0,dropped=0;
	int w=screen->w,h=w*media.height/media.width;
	if (h>screen->h) { h=screen->h; w=h*media.width/media.height; }
	SDL_FillRect(screen,nullptr,0);
	while (!pax_exit_requested) {
		pax_stratagus_input_poll();
		SDL_Event event{}; bool skip=false;
		while (SDL_PollEvent(&event)) {
			if (event.type==SDL_QUIT) pax_exit_requested=1;
			if (event.type==SDL_KEYDOWN || event.type==SDL_MOUSEBUTTONUP) skip=true;
		}
		if (skip || pax_exit_requested) break;
		const unsigned frame=(SDL_GetTicks()-start)/media.interval;
		if (frame>=media.frames) break;
		if (frame==shown) { SDL_Delay(2); continue; }
		if (!media.decode(frame)) { media.close(); pax_stratagus_audio_music_file(""); return false; }
		media.draw(screen,(screen->w-w)/2,(screen->h-h)/2,frame,w,h);
		pax_stratagus_video_present565(screen->pixels,screen->w,screen->h);
		if (!presented && _pax_svc->log_printf)
			_pax_svc->log_printf("Warcraft: first movie frame presented %s %ux%u\n",name,media.width,media.height);
		dropped+=shown==~0u ? frame:frame-shown-1; shown=frame; ++presented;
	}
	pax_stratagus_audio_music_file("");
	if (_pax_svc->log_printf) _pax_svc->log_printf("Warcraft: video playback %s frames=%u/%u dropped=%u\n",name,presented,media.frames,dropped);
	media.close(); return !pax_exit_requested;
}

int pax_stratagus_menu_run(SDL_Surface *screen,bool showIntro)
{
	if (!screen || !screen->pixels || screen->format->BytesPerPixel!=2 || screen->w!=800 || screen->h!=480) return 0;
	Menu menu{}; menu.screen=screen;
	if (!menu.init()) { if (_pax_svc->log_printf) _pax_svc->log_printf("Stargus: menu asset pack missing or invalid\n"); menu.close(); return 0; }
	const int result=menu.run(showIntro);
	if (result>0 && !menu.campaignText(result)) { menu.close(); return 0; }
	if (result>0) menu.loading(result); menu.close();
	if (result>0) pax_stratagus_audio_music(0);
	return result;
}
const char *pax_stratagus_menu_save_name() { return chosenSaveName; }

void pax_stratagus_portrait_close() { portrait.close(); portrait=Media{}; portraitIndex=-1; }
bool pax_stratagus_portrait_draw(SDL_Surface *screen,const char *ident,int x,int y)
{
#ifdef PAX_STRATAGUS_WARCRAFT
	// These companions contain StarCraft portraits only. Warcraft's native
	// portrait/icon path must handle its units; retrying an absent advisor
	// every frame scans its large SD movie pack and stalls HUD rendering.
	return false;
#else
	const char *ids[]={"unit-terran-marine","unit-terran-scv","unit-hero-jim-raynor-vulture","unit-zerg-zergling"};
	const char *files[]={"videos/portrait/tmarine/tmafid00.mng","videos/portrait/tscv/tscfid00.mng",
		"videos/portrait/ujim/urafid00.mng","videos/portrait/zzergl/zzefid00.mng","videos/portrait/tadvisor/tadfid00.mng"};
	int index=4; for (int i=0;i<4;++i) if (!strcmp(ident,ids[i])) index=i;
	if (index!=portraitIndex) {
		pax_stratagus_portrait_close();
		if (!portrait.open(files[index])) return false;
		portraitIndex=index; portraitStart=SDL_GetTicks();
	}
	if (!portrait.decode(portrait.frame(SDL_GetTicks()-portraitStart))) return false;
	portrait.draw(screen,x,y,portrait.current,60,56); return true;
#endif
}

void pax_stratagus_menu_set_tutorial(bool tutorial) { gameIsTutorial=tutorial; }

void pax_stratagus_results_show(SDL_Surface *screen)
{
	if (!pax_results.outcome || !screen) return;
	pax_stratagus_audio_view(0,0,0,0);
	Menu art{}; art.screen=screen; art.fontCellW=10; art.fontCellH=13;
	Media backdrop{};
	const bool victory=pax_results.outcome==1;
	const bool font=art.font.open("graphics/ui/fonts/font10.png") && art.initFont();
	backdrop.open(victory?"graphics/terran/victory screen.png":"graphics/terran/defeat screen.png");
	art.button.open("graphics/ui/readyt/butterr.png");
	art.cursor.open("graphics/ui/cursors/arrow.png");
	pax_stratagus_audio_music(2);
	if (_pax_svc->log_printf) _pax_svc->log_printf("Stargus: results opened outcome=%d players=%d time=%d\n",
		pax_results.outcome,pax_results.count,pax_results.seconds);
	bool done=false; const SDL_Rect button{580,430,172,35};
	while (!done && !pax_exit_requested) {
		SDL_FillRect(screen,nullptr,0); backdrop.draw(screen,0,0,0,800,480);
		for (int y=116;y<404;++y) {
			auto *row=(Uint16 *)((Uint8 *)screen->pixels+y*screen->pitch);
			for (int x=16;x<784;++x) row[x]=((row[x]&0xf7de)>>1);
		}
		if (font) {
			art.text(victory?"Victory!":pax_results.outcome==2?"Defeat!":"Draw!",400,48,0xffe0,false,true,-1,150);
			char elapsed[48]; snprintf(elapsed,sizeof(elapsed),"Elapsed time: %d:%02d",pax_results.seconds/60,pax_results.seconds%60);
			art.text(elapsed,400,80,0xffff,false,true);
			const char *headings[]={"Player","Units","Buildings","Minerals","Gas","Kills","Razings"};
			const int x[]={30,275,360,460,555,645,730};
			for (int c=0;c<7;++c) art.text(headings[c],x[c],135,0xffe0,false,c!=0,-1,125);
			for (int r=0;r<pax_results.count;++r) {
				const auto &row=pax_results.rows[r]; const int values[]={row.units,row.buildings,row.minerals,row.gas,row.kills,row.razings};
				art.text(row.name,x[0],166+r*29,0xffff,false,false,-1,125);
				for (int c=0;c<6;++c) {char n[24]; snprintf(n,sizeof(n),"%d",values[c]); art.text(n,x[c+1],166+r*29,0xffff,false,true,-1,125);}
			}
			art.button.draw(screen,button.x,button.y,0,button.w,button.h,true);
			art.text("Continue",666,440,0xffe0,false,true,-1,150);
		}
		int mx=0,my=0; SDL_GetMouseState(&mx,&my); art.cursor.draw(screen,mx,my,0,32,32);
		pax_stratagus_video_present565(screen->pixels,800,480);
#ifdef STRATAGUS_PAX_RESULTS_TEST
		done=true;
#endif
		SDL_Event event{};
		while (SDL_PollEvent(&event)) {
			if (event.type==SDL_QUIT) {pax_exit_requested=1;done=true;}
			if (event.type==SDL_KEYDOWN && (event.key.keysym.sym==SDLK_c || event.key.keysym.sym==SDLK_RETURN ||
				event.key.keysym.sym==SDLK_ESCAPE || event.key.keysym.sym==SDLK_SPACE)) done=true;
			if (event.type==SDL_MOUSEBUTTONUP && event.button.button==SDL_BUTTON_LEFT &&
				event.button.x>=button.x && event.button.x<button.x+button.w && event.button.y>=button.y && event.button.y<button.y+button.h) done=true;
		}
		pax_stratagus_input_poll(); SDL_Delay(20);
	}
	backdrop.close(); art.close();
	if (_pax_svc->log_printf) _pax_svc->log_printf("Stargus: results Continue\n");
}

int pax_stratagus_game_menu_run(SDL_Surface *screen,int page)
{
	if (!screen || screen->w!=800 || screen->h!=480 || screen->format->BytesPerPixel!=2) return 0;
	// The game heap can have ample total free memory but no contiguous
	// 768000-byte block. Small strips preserve the complete paused backdrop
	// without depending on a second full-canvas allocation.
	GameSnapshot snapshot{};
	GameMenu menu{};
	if (!menu.init(screen)) {
		snapshot.close(); menu.close();
		if (_pax_svc->log_printf) _pax_svc->log_printf("Stargus: Game Menu could not allocate/open its assets\n");
		return 0;
	}
	if (!snapshot.capture(screen) && _pax_svc->log_printf)
		_pax_svc->log_printf("Stargus: Game Menu using low-memory backdrop\n");
	menu.page=page; if (page==1 || page==2) menu.refreshSaves(); menu.editing=page==1; menu.layout();
	if (_pax_svc->log_printf) _pax_svc->log_printf("Stargus: Game Menu opened\n");
	int result=-1;
	while (!pax_exit_requested && result<0) {
		SDL_Event event{};
		for (int n=0;n<64 && result<0 && SDL_PollEvent(&event);++n) {
			int choose=-1;
			if (event.type==SDL_QUIT) { result=0; break; }
			if (event.type==SDL_MOUSEMOTION) menu.selected=menu.art.hit(event.motion.x,event.motion.y);
			else if (event.type==SDL_MOUSEBUTTONDOWN && event.button.button==SDL_BUTTON_LEFT)
				menu.pressed=menu.art.hit(event.button.x,event.button.y);
			else if (event.type==SDL_MOUSEBUTTONUP && event.button.button==SDL_BUTTON_LEFT) {
				int target=menu.art.hit(event.button.x,event.button.y);
				if (target==menu.pressed) choose=target; menu.pressed=-1;
			} else if (event.type==SDL_KEYDOWN) {
				const auto key=event.key.keysym.sym;
				if (menu.page==1 && menu.editing && key!=SDLK_ESCAPE) {
					const size_t length=strlen(menu.filename);
					if (key==SDLK_RETURN) { menu.editing=false; menu.selected=1+menu.visibleSaves; }
					else if (key==SDLK_BACKSPACE) { if (length) menu.filename[length-1]=0; }
					else if (key>=32 && key<127 && length<56) {
						const bool valid=(key>='a' && key<='z') || (key>='0' && key<='9') || key==' ' || key=='_' || key=='-' || key=='.';
						if (valid) { menu.filename[length]=char(key); menu.filename[length+1]=0; }
					}
					continue;
				}
				if (key==SDLK_ESCAPE) { if (menu.page) { menu.page=0; menu.layout(); } else result=0; }
				else if (key==SDLK_UP || key==SDLK_DOWN || key==SDLK_TAB)
					menu.selected=(menu.selected+(key==SDLK_UP?-1:1)+menu.art.count)%menu.art.count;
				else if (key==SDLK_RETURN || key==SDLK_SPACE) choose=menu.selected<0?0:menu.selected;
				else for (int i=0;i<menu.art.count;++i) if (menu.art.buttons[i].hotkey==key) choose=i;
			}
			if (choose>=0) result=menu.activate(choose);
		}
		pax_stratagus_input_poll(); menu.draw(snapshot); SDL_Delay(20);
	}
	snapshot.restore(screen); snapshot.close(); menu.close();
	if (_pax_svc->log_printf) _pax_svc->log_printf("Stargus: Game Menu closed end=%d\n",result==1);
	return result<0?0:result;
}
