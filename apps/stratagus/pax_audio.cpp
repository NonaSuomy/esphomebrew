#define PAX_APP_SIDE 1
#include "pax_platform.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <initializer_list>

static double pax_vorbis_sin(double angle) {
	const double pi=3.14159265358979323846;
	while(angle>pi) angle-=2*pi;while(angle<-pi) angle+=2*pi;
	if(angle>pi/2) angle=pi-angle;if(angle<-pi/2) angle=-pi-angle;
	double sum=angle,term=angle;
	for(int n=1;n<=8;++n) {term*=-angle*angle/((2*n)*(2*n+1));sum+=term;}return sum;
}
static double pax_vorbis_cos(double angle) {return pax_vorbis_sin(angle+1.57079632679489661923);}
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_FAST_SCALED_FLOAT
#define STB_VORBIS_MAX_CHANNELS 2
#include "pax_stb_vorbis.h"
#include "pax_sound_bank.h"
#ifdef PAX_STRATAGUS_MIDI_WAR1
#include "pax_midi.h"
#endif
#ifdef PAX_STRATAGUS_WARCRAFT
#include "pax_resample48.h"
static int16_t s_device_pcm[1120*2];
#endif

static void *s_audio_task;
static void *s_decode_task;
static int s_decode_alive;
static int s_audio_alive,s_audio_stop,s_music_track;
static int s_camera[4];
static short s_pcm[1024*2];
static char *s_music_memory,*s_voice_memory;
static constexpr int decoder_bytes=192*1024;
static bool s_prepared;
static int s_music_lock, s_music_revision;
static char s_music_path[256];
#ifdef PAX_STRATAGUS_MIDI_WAR1
static char s_soundfont_path[256] = PAX_STRATAGUS_DATA_ROOT "/music/TimGM6mb.sf2";
extern "C" int pax_stratagus_midi_soundfont(const char *path) {
	// Only game-local SF2 banks; reject malformed/oversized banks before the
	// audio worker attempts allocation. File size is not a RAM-use guarantee.
	if(!path || strncmp(path,"music/",6) || strchr(path+6,'/') || strchr(path,'\\') || strstr(path,"..")) return 0;
	const size_t length=strlen(path);
	if(length<10 || pax_strncasecmp(path+length-4,".sf2",4)) return 0;
	char resolved[256];
	if(snprintf(resolved,sizeof(resolved),"%s/%s",PAX_STRATAGUS_DATA_ROOT,path)>=int(sizeof(resolved))) return 0;
	FILE *file=fopen(resolved,"rb"); if(!file) return 0;
	char header[12]; const bool valid=fread(header,1,12,file)==12 && !memcmp(header,"RIFF",4) && !memcmp(header+8,"sfbk",4);
	fseek(file,0,SEEK_END); const long bytes=ftell(file); fclose(file);
	if(!valid || bytes<12 || bytes>8*1024*1024) return 0;
	while(__atomic_exchange_n(&s_music_lock,1,__ATOMIC_ACQUIRE)) _pax_svc->delay_ms(1);
	if(strcmp(s_soundfont_path,resolved)) {
		strcpy(s_soundfont_path,resolved);
		const size_t n=strlen(s_music_path);
		if(n>4 && !strcmp(s_music_path+n-4,".mid")) ++s_music_revision;
	}
	__atomic_store_n(&s_music_lock,0,__ATOMIC_RELEASE);
	return 1;
}
#endif
static int s_music_playing, s_music_volume=85;
extern "C" int pax_stratagus_audio_music_playing() { return __atomic_load_n(&s_music_playing,__ATOMIC_ACQUIRE); }
extern "C" void pax_stratagus_audio_music_volume(int volume) {
	__atomic_store_n(&s_music_volume,volume<0?0:volume>255?255:volume,__ATOMIC_RELEASE);
}

void pax_stratagus_audio_music(int track) {__atomic_store_n(&s_music_track,track,__ATOMIC_RELEASE);}
int pax_stratagus_audio_music_file(const char *path) {
	char resolved[256]{};
	if(path && *path) {
		if(strstr(path,"..")) return -1;
		const unsigned rootLength=strlen(PAX_STRATAGUS_DATA_ROOT);
		if(!strncmp(path,PAX_STRATAGUS_DATA_ROOT,rootLength) && path[rootLength]=='/') path+=rootLength+1;
		if(*path=='/' || strlen(path)+rootLength+6>=sizeof(resolved)) return -1;
		snprintf(resolved,sizeof(resolved),"%s/%s",PAX_STRATAGUS_DATA_ROOT,path);
		const unsigned length=strlen(resolved);
		bool isMidi=false;
#ifdef PAX_STRATAGUS_MIDI_WAR1
		isMidi=length>4 && !strcmp(resolved+length-4,".mid");
#endif
		if(length>4 && (!strcmp(resolved+length-4,".wav") || (!isMidi && !strcmp(resolved+length-4,".mid"))))
			strcpy(resolved+length-4,".ogg");
		if(!isMidi && (length<4 || strcmp(resolved+length-4,".ogg"))) return -1;
#ifdef PAX_STRATAGUS_MIDI_WAR1
		if(isMidi) {
			char font[256];
			while(__atomic_exchange_n(&s_music_lock,1,__ATOMIC_ACQUIRE)) _pax_svc->delay_ms(1);
			strcpy(font,s_soundfont_path);
			__atomic_store_n(&s_music_lock,0,__ATOMIC_RELEASE);
			FILE *sf=fopen(font,"rb");
			if(!sf) return -1;
			fclose(sf);
		}
#endif
		FILE *probe=fopen(resolved,"rb"); if(!probe) return -1; fclose(probe);
	}
	while(__atomic_exchange_n(&s_music_lock,1,__ATOMIC_ACQUIRE)) _pax_svc->delay_ms(1);
	if (!strcmp(s_music_path,resolved)) {
		__atomic_store_n(&s_music_lock,0,__ATOMIC_RELEASE); return 0;
	}
	strcpy(s_music_path,resolved); ++s_music_revision;
	__atomic_store_n(&s_music_playing,*resolved?1:0,__ATOMIC_RELEASE);
	__atomic_store_n(&s_music_lock,0,__ATOMIC_RELEASE);
	return 0;
}
void pax_stratagus_audio_prepare() {
	if(!_pax_svc || !_pax_svc->mem_caps_alloc) return;
	if(!s_music_memory) s_music_memory=(char *)_pax_svc->mem_caps_alloc(decoder_bytes,PAX_MEM_CAP_SPIRAM);
	if(!s_voice_memory) s_voice_memory=(char *)_pax_svc->mem_caps_alloc(decoder_bytes,PAX_MEM_CAP_SPIRAM);
}
void pax_stratagus_audio_precache() {
	if(s_prepared || !s_voice_memory) return;
	stb_vorbis_alloc arena{s_voice_memory,decoder_bytes};PaxSoundBank::prepare(&arena);s_prepared=true;
}
void pax_stratagus_audio_view(int x,int y,int width,int height) {
	const int values[]={x,y,width,height};for(int i=0;i<4;++i) __atomic_store_n(&s_camera[i],values[i],__ATOMIC_RELEASE);
}
extern "C" int pax_stratagus_audio_sound_id(const char *name) {return PaxSoundBank::find(name);}
extern "C" void pax_stratagus_audio_play_id(int id,int x,int y,int volume,int flags) {
	if(id>0 && unsigned(id)<=PaxSoundBank::groupCount && flags==2 && !strncmp(pax_sound_groups[id-1].name,"pax-",5)) flags=0;
	PaxSoundBank::push(id,x,y,volume,flags);
}
extern "C" void pax_stratagus_audio_named(const char *name,int x,int y,int volume,int flags) {
	const int id=PaxSoundBank::find(name);
	if(id) PaxSoundBank::push(id,x,y,volume,flags);
	else {static unsigned missing;if(missing++<20 && _pax_svc->log_printf) _pax_svc->log_printf("Stargus: unregistered sound event=%s\n",name);}
}
extern "C" void pax_stratagus_audio_effect_at(const char *name,int x,int y) {pax_stratagus_audio_named(name,x,y,255,1);}
extern "C" void pax_stratagus_audio_effect(const char *name) {pax_stratagus_audio_named(name,0,0,255,2);}
void pax_stratagus_audio_voice(const char *unit,int acknowledge) {
	char name[160];snprintf(name,sizeof(name),"%s-sound-%s",unit,acknowledge?"yes":"selected");
	int id=PaxSoundBank::find(name);
	if(!id && !acknowledge) {snprintf(name,sizeof(name),"%s-sound-what",unit);id=PaxSoundBank::find(name);}
	if(id) PaxSoundBank::push(id,0,0,255,acknowledge?0:4);
}
void pax_stratagus_audio_unit_event(const char *unit,int voice,int own,int x,int y) {
	const char *race=!strncmp(unit,"unit-zerg-",10)?"zerg":!strncmp(unit,"unit-protoss-",13)?"protoss":"terran";
	char name[160];int id=0,flags=0;
	if(voice==4) {snprintf(name,sizeof(name),"%s-death",!strncmp(unit,"unit-",5)?unit+5:unit);id=PaxSoundBank::find(name);flags=1;
		if(!id && !strcmp(unit,"unit-hero-jim-raynor-vulture")) id=PaxSoundBank::find("terran-vulture-death");
		if(!id) id=PaxSoundBank::find("explosion-large");
	} else if(!own) return;
	else if(voice==2) {snprintf(name,sizeof(name),"%s-sound-ready",unit);id=PaxSoundBank::find(name);}
	else if(voice==5 && !strcmp(unit,"unit-terran-scv")) id=PaxSoundBank::find("terran-scv-done");
	else if(voice==3) {static int64_t last=-5000000;const auto now=_pax_svc->get_time_us();if(now-last<5000000) return;last=now;
		snprintf(name,sizeof(name),"%s-%s-attacked",race,strstr(unit,"command-center")||strstr(unit,"barracks")||strstr(unit,"depot")?"base":"units");id=PaxSoundBank::find(name);
	} else if(voice==7) {snprintf(name,sizeof(name),"sounds/unit/misc/%s",!strcmp(race,"terran")?"tdrtra00.ogg":!strcmp(race,"zerg")?"zovtra00.ogg":"pshtra00.ogg");id=PaxSoundBank::find(name);flags=1;}
	else if(voice>=8) {pax_stratagus_audio_voice(unit,1);return;}
	if(id) PaxSoundBank::push(id,x,y,255,flags);
}

static void sound_decode_worker(void *) {
	while(!__atomic_load_n(&s_audio_stop,__ATOMIC_ACQUIRE) && !pax_exit_requested) {
		PaxSoundBank::decode_work(); _pax_svc->delay_ms(5);
	}
	__atomic_store_n(&s_decode_alive,0,__ATOMIC_RELEASE);
	for(;;) _pax_svc->delay_ms(100);
}
static void audio_worker(void *) {
	stb_vorbis_alloc music_alloc{s_music_memory,decoder_bytes};
	stb_vorbis *music=nullptr;int track=-1,error=0;
#ifdef PAX_STRATAGUS_MIDI_WAR1
	PaxMidi midi;
#endif
#ifdef PAX_STRATAGUS_WARCRAFT
	PaxResample48 deviceRate;
	_pax_svc->audio_init(48000);
#else
	_pax_svc->audio_init(22050);
#endif
	int64_t anchor=_pax_svc->get_time_us(),sent=0,stats_start=anchor,decode_us=0,decode_max_us=0,stats_frames=0;
	unsigned late_blocks=0,blocks=0;
	while(!__atomic_load_n(&s_audio_stop,__ATOMIC_ACQUIRE) && !pax_exit_requested) {
		const int64_t begin=_pax_svc->get_time_us();
#ifdef PAX_STRATAGUS_WARCRAFT
		int requested=track; char musicPath[256]{};
#ifdef PAX_STRATAGUS_MIDI_WAR1
		char soundfontPath[256]{};
#endif
		if(!__atomic_exchange_n(&s_music_lock,1,__ATOMIC_ACQUIRE)) {
			requested=s_music_revision; strcpy(musicPath,s_music_path);
#ifdef PAX_STRATAGUS_MIDI_WAR1
			strcpy(soundfontPath,s_soundfont_path);
#endif
			__atomic_store_n(&s_music_lock,0,__ATOMIC_RELEASE);
		}
#else
		const int requested=__atomic_load_n(&s_music_track,__ATOMIC_ACQUIRE);
#endif
		if(requested!=track) {
			if(music) stb_vorbis_close(music);music=nullptr;track=requested;
#ifdef PAX_STRATAGUS_MIDI_WAR1
			const size_t pathLength=strlen(musicPath);
			const bool requestMidi=pathLength>4 && !strcmp(musicPath+pathLength-4,".mid");
			if(!requestMidi) midi.close();
			if(requestMidi && !midi.open(musicPath,soundfontPath,_pax_svc->get_time_us)) {
				_pax_svc->log_printf("Warcraft: MIDI initialization failed; falling back to OGG\n");
				strcpy(musicPath+pathLength-4,".ogg");
			}
#endif
			if(s_music_memory) {
#ifdef PAX_STRATAGUS_WARCRAFT
#ifdef PAX_STRATAGUS_MIDI_WAR1
				if(!midi.active())
#endif
				if(*musicPath) music=stb_vorbis_open_filename(musicPath,&error,&music_alloc);
#else
                music=stb_vorbis_open_filename(track==1 ? PAX_STRATAGUS_DATA_ROOT "/media/stargus_intro.ogg" :
                    track==2 ? PAX_STRATAGUS_DATA_ROOT "/sounds/music/title.ogg" :
                    PAX_STRATAGUS_DATA_ROOT "/sounds/music/terran/1.ogg", &error, &music_alloc);
#endif
			}
			if(music && stb_vorbis_get_info(music).sample_rate!=22050) {stb_vorbis_close(music);music=nullptr;}
#ifdef PAX_STRATAGUS_WARCRAFT
			__atomic_store_n(&s_music_playing,music?1:0,__ATOMIC_RELEASE);
#ifdef PAX_STRATAGUS_MIDI_WAR1
			if(midi.active()) {
				__atomic_store_n(&s_music_playing,1,__ATOMIC_RELEASE);
				_pax_svc->log_printf("Warcraft: FluidSynth MIDI playing %s soundfont=%s (16 voices, linear interpolation, 16000 Hz -> 22050 Hz)\n",musicPath,soundfontPath);
				_pax_svc->log_printf("Warcraft: MIDI init settings=%d synth=%d font=%d player=%d ms\n",int(midi.timing.settings/1000),int(midi.timing.synth/1000),int(midi.timing.font/1000),int(midi.timing.player/1000));
			}
#endif
#endif
			if(_pax_svc->log_printf) _pax_svc->log_printf("Stargus: music track=%d decoder=%p error=%d\n",track,music,error);
		}
		memset(s_pcm,0,sizeof(s_pcm));int frames=music?stb_vorbis_get_samples_short_interleaved(music,2,s_pcm,1024):512;
#ifdef PAX_STRATAGUS_MIDI_WAR1
		if(midi.active()) frames=midi.render(s_pcm,512);
#endif
		if(!frames && music) {
#ifndef PAX_STRATAGUS_WARCRAFT
			if(track==1) {stb_vorbis_close(music);music=nullptr;frames=512;} else
#endif
			{stb_vorbis_seek_start(music);continue;}}
#ifdef PAX_STRATAGUS_WARCRAFT
		const int volume=__atomic_load_n(&s_music_volume,__ATOMIC_ACQUIRE);
		for(int i=0;i<frames*2;++i) s_pcm[i]=int(s_pcm[i])*volume/255;
#else
		for(int i=0;i<frames*2;++i) s_pcm[i]/=3;
#endif
		int camera[4];for(int i=0;i<4;++i) camera[i]=__atomic_load_n(&s_camera[i],__ATOMIC_ACQUIRE);
		PaxSoundBank::mix(s_pcm,frames,camera[0],camera[1],camera[2],camera[3]);
		const int64_t now=_pax_svc->get_time_us(),decoded=now-begin;decode_us+=decoded;if(decoded>decode_max_us) decode_max_us=decoded;
		const int64_t ahead=sent*1000000/22050-(now-anchor);
		if(ahead<0) ++late_blocks;
		if(ahead<-250000) {anchor=now;sent=0;} else if(ahead>120000) {const int ms=(ahead-120000)/1000+1;_pax_svc->delay_ms(ms<10?10:ms);}
#ifdef PAX_STRATAGUS_WARCRAFT
		const size_t deviceFrames=deviceRate.convert(s_pcm,frames,s_device_pcm,1120);
		if (deviceFrames) _pax_svc->audio_submit(s_device_pcm,deviceFrames);
#else
		_pax_svc->audio_submit(s_pcm,frames);
#endif
		sent+=frames;stats_frames+=frames;
		if(++blocks==1 && _pax_svc->log_printf) _pax_svc->log_printf("Stargus: audio submitted %d frames at 22050 Hz\n",frames);
		const int64_t end=_pax_svc->get_time_us();
		if(end-stats_start>=3000000 && _pax_svc->log_printf) {
			_pax_svc->log_printf("Stargus: audio rate=%d target=22050 decode=%d%% max=%d ms late=%u catalog-plays=%u cache=%u misses=%u dropped=%u\n",
				int(stats_frames*1000000/(end-stats_start)),int(decode_us*100/(end-stats_start)),int(decode_max_us/1000),late_blocks,
				PaxSoundBank::plays,PaxSoundBank::bytes,PaxSoundBank::misses,__atomic_load_n(&PaxSoundBank::dropped,__ATOMIC_RELAXED));
			stats_start=end;stats_frames=decode_us=decode_max_us=0;late_blocks=0;
		}
	}
	while(__atomic_load_n(&s_decode_alive,__ATOMIC_ACQUIRE)) _pax_svc->delay_ms(5);
	if(music) stb_vorbis_close(music);PaxSoundBank::close();
#ifdef PAX_STRATAGUS_MIDI_WAR1
	midi.close();
#endif
	__atomic_store_n(&s_music_playing,0,__ATOMIC_RELEASE);
	if(s_music_memory) _pax_svc->mem_free(s_music_memory);if(s_voice_memory) _pax_svc->mem_free(s_voice_memory);
	s_music_memory=s_voice_memory=nullptr;s_prepared=false;
	__atomic_store_n(&s_audio_alive,0,__ATOMIC_RELEASE);for(;;) _pax_svc->delay_ms(100);
}
void pax_stratagus_audio_start() {
	if(s_audio_task || !_pax_svc || !_pax_svc->audio_init || !_pax_svc->audio_submit || !_pax_svc->task_create || !_pax_svc->task_delete) return;
	pax_stratagus_audio_prepare();pax_stratagus_audio_precache();
	__atomic_store_n(&s_audio_stop,0,__ATOMIC_RELEASE);__atomic_store_n(&s_audio_alive,1,__ATOMIC_RELEASE);
	__atomic_store_n(&s_decode_alive,1,__ATOMIC_RELEASE);
	if(_pax_svc->task_create(sound_decode_worker,"stargus_sfx",8192,nullptr,2,&s_decode_task,1)) {
		__atomic_store_n(&s_decode_alive,0,__ATOMIC_RELEASE);
	} else {PaxSoundBank::asyncDecode=true;if(_pax_svc->log_printf) _pax_svc->log_printf("Stargus: asynchronous FX decoder active\n");}
	// Match the proven Quake audio scheduling: the short decode/mix burst
	// must outrank rendering/network work, then block while the speaker drains.
#ifdef PAX_STRATAGUS_WARCRAFT
	constexpr int mixerPriority=8;
#else
	constexpr int mixerPriority=3;
#endif
#ifdef PAX_STRATAGUS_MIDI_WAR1
	constexpr unsigned mixerStack=32768;
#else
	constexpr unsigned mixerStack=8192;
#endif
	if(_pax_svc->task_create(audio_worker,"stargus_audio",mixerStack,nullptr,mixerPriority,&s_audio_task,1)) {
		__atomic_store_n(&s_audio_stop,1,__ATOMIC_RELEASE);
		__atomic_store_n(&s_audio_alive,0,__ATOMIC_RELEASE);if(_pax_svc->log_printf) _pax_svc->log_printf("Stargus: audio worker unavailable\n");
	}
}
void pax_stratagus_audio_stop() {
	__atomic_store_n(&s_audio_stop,1,__ATOMIC_RELEASE);while(__atomic_load_n(&s_audio_alive,__ATOMIC_ACQUIRE)) _pax_svc->delay_ms(5);
	while(__atomic_load_n(&s_decode_alive,__ATOMIC_ACQUIRE)) _pax_svc->delay_ms(5);
	if(s_audio_task) _pax_svc->task_delete(s_audio_task);s_audio_task=nullptr;
	if(s_decode_task) _pax_svc->task_delete(s_decode_task);s_decode_task=nullptr;PaxSoundBank::close();
	if(s_music_memory) _pax_svc->mem_free(s_music_memory);if(s_voice_memory) _pax_svc->mem_free(s_voice_memory);
	s_music_memory=s_voice_memory=nullptr;s_prepared=false;
}
