#pragma once
#ifdef PAX_STRATAGUS_WARCRAFT
#include "pax_sound_catalog.h"
#else
#include "pax_stargus_sound_catalog.h"
#endif
#include "pax_audio_spatial.h"
#include "pax_audio_ramp.h"

namespace PaxSoundBank {
constexpr unsigned fileCount=sizeof(pax_sound_files)/sizeof(*pax_sound_files);
constexpr unsigned groupCount=sizeof(pax_sound_groups)/sizeof(*pax_sound_groups);
constexpr unsigned budget=1024*1024;
struct Clip { int file=-1; short *pcm=nullptr; int frames=0; unsigned used=0; int divisor=1; };
struct Request { int group,x,y,volume,flags; int file=-1; };
struct Voice { int clip=-1,frame=0,group=0,x=0,y=0,volume=255,flags=0; PaxAudioGain prior{}; };
static Clip clips[24];
static Voice voices[10];
static PaxVoiceRamp ramps[10];
static Request requests[32];
static unsigned writeIndex,readIndex,bytes,clock,plays,misses,dropped;
static unsigned variants[groupCount]{};
static bool reported[fileCount]{};
static bool asyncDecode;
static int jobState,jobFile,failedFile=-1;
static Clip decodedClip;
static int lastSelection;
static unsigned selectionSource;
static unsigned selectionCount;
static int64_t selectionTime;
static short source[1024];
static short sourceRight[1024];
static stb_vorbis_alloc arenaStorage;
static stb_vorbis_alloc *arena;

inline int find(const char *name) {
	if (!name || !*name) return 0;
	unsigned low=0,high=groupCount;
	while(low<high) {const unsigned mid=(low+high)/2; const int cmp=strcmp(name,pax_sound_groups[mid].name);
		if (!cmp) return mid+1; if(cmp<0) high=mid; else low=mid+1;}
	return 0;
}
inline bool push(int group,int x,int y,int volume,int flags) {
	if (group<=0 || unsigned(group)>groupCount || volume<=0) return false;
	const unsigned w=__atomic_load_n(&writeIndex,__ATOMIC_RELAXED);
	const unsigned r=__atomic_load_n(&readIndex,__ATOMIC_ACQUIRE);
	if (w-r>=32) {__atomic_fetch_add(&dropped,1,__ATOMIC_RELAXED); return false;}
	requests[w%32]={group,x,y,volume>255?255:volume,flags};
	__atomic_store_n(&writeIndex,w+1,__ATOMIC_RELEASE); return true;
}
inline bool active(unsigned slot) {for (auto &v:voices) if(v.clip==int(slot)) return true; return false;}
inline void release(unsigned slot) {
	auto &c=clips[slot]; if(c.pcm) {_pax_svc->mem_free(c.pcm); bytes-=c.frames*2;}
	c={};
}
inline int oldest() {
	int result=-1;
	for (unsigned i=0;i<24;++i) if(clips[i].pcm && !active(i) && (result<0 || clips[i].used<clips[result].used)) result=i;
	return result;
}
inline Clip decode(unsigned file) {
	Clip result;
	if(file>=fileCount || !arena) return result;
	char path[256]; snprintf(path,sizeof(path),"%s/%s",PAX_STRATAGUS_DATA_ROOT,pax_sound_files[file]);
	int error=0; auto *decoder=stb_vorbis_open_filename(path,&error,arena);
	if(!decoder) {if(_pax_svc->log_printf) _pax_svc->log_printf("Stargus: sound load failed %s error=%d\n",pax_sound_files[file],error);return result;}
	const auto info=stb_vorbis_get_info(decoder);
	const unsigned rate=info.sample_rate;
	const unsigned length=stb_vorbis_stream_length_in_samples(decoder);
	// Preserve 22.05-kHz detail for short FX; only long voices use half rate.
	const unsigned cacheRate=rate && uint64_t(length)*22050/rate>131072?11025:22050;
	const unsigned count=rate?uint64_t(length)*cacheRate/rate:0;
	if(rate<8000 || rate>48000 || !count || count>cacheRate*30 || count*2>budget) {stb_vorbis_close(decoder);return result;}
	short *pcm=(short *)_pax_svc->mem_caps_alloc(count*2,PAX_MEM_CAP_SPIRAM);
	if(!pcm) {stb_vorbis_close(decoder);return result;}
	stb_vorbis_seek_start(decoder);
	int buffered=0,at=0;unsigned position=0;
	auto next=[&]() -> short {
		if(at>=buffered) {
			short *channels[]={source,sourceRight};buffered=stb_vorbis_get_samples_short(decoder,info.channels,channels,1024);at=0;
			if(info.channels==2) for(int i=0;i<buffered;++i) source[i]=(int(source[i])+sourceRight[i])/2;
		}
		return buffered?source[at++]:0;
	};
	short a=next(),b=next();
	for(unsigned i=0;i<count;++i) {
		const uint64_t target=uint64_t(i)*rate; const unsigned index=target/cacheRate,frac=target%cacheRate;
		while(position<index) {a=b;b=next();++position;}
		pcm[i]=(int(a)*int(cacheRate-frac)+int(b)*int(frac))/cacheRate;
	}
	stb_vorbis_close(decoder);
	return {int(file),pcm,int(count),0,int(22050/cacheRate)};
}
inline int install(Clip &result) {
	if(!result.pcm) return -2;
	while(bytes+result.frames*2>budget) {const int victim=oldest();if(victim<0) return -1;release(victim);}
	int slot=-1;for(unsigned i=0;i<24;++i) if(!clips[i].pcm) {slot=i;break;}
	if(slot<0) {slot=oldest();if(slot>=0) release(slot);}
	if(slot<0) return -1;
	result.used=++clock;clips[slot]=result;bytes+=result.frames*2;result={};return slot;
}
// Decode task owns only the unpublished job. Cache/voice state stays on mixer.
inline void decode_work() {
	if(__atomic_load_n(&jobState,__ATOMIC_ACQUIRE)!=1) return;
	decodedClip=decode(jobFile);
	__atomic_store_n(&jobState,2,__ATOMIC_RELEASE);
}
inline int load(unsigned file) {
	for (unsigned i=0;i<24;++i) if(clips[i].file==int(file) && clips[i].pcm) {clips[i].used=++clock;return i;}
	if(asyncDecode) {
		const int state=__atomic_load_n(&jobState,__ATOMIC_ACQUIRE);
		if(state==2) {
			const int completedFile=jobFile;
			const int slot=install(decodedClip);
			if(slot==-1) return -1;
			if(slot==-2) {failedFile=completedFile;++misses;}
			__atomic_store_n(&jobState,0,__ATOMIC_RELEASE);
			if(completedFile==int(file)) return slot;
		}
		if(int(file)==failedFile) return -2;
		if(__atomic_load_n(&jobState,__ATOMIC_ACQUIRE)==0) {jobFile=file;__atomic_store_n(&jobState,1,__ATOMIC_RELEASE);}
		return -1;
	}
	Clip result=decode(file);const int slot=install(result);
	if(slot<0) {if(result.pcm) _pax_svc->mem_free(result.pcm);++misses;}return slot;
}
inline void prepare(stb_vorbis_alloc *memory) {
	arenaStorage=*memory; arena=&arenaStorage;
	for (const char *name:{"button","buzz","building-placed","terran-scv-attack","terran-marine-attack"}) {
		const int id=find(name);if(id && pax_sound_groups[id-1].count) load(pax_sound_members[pax_sound_groups[id-1].offset]);
	}
	if(_pax_svc->log_printf) _pax_svc->log_printf("Stargus: catalog audio ready groups=%u files=%u cache=%u/%u bytes\n",groupCount,fileCount,bytes,budget);
}
inline void mix(short *pcm,int frames,int left,int top,int width,int height) {
	const unsigned w=__atomic_load_n(&writeIndex,__ATOMIC_ACQUIRE);
	unsigned r=__atomic_load_n(&readIndex,__ATOMIC_RELAXED);
	// Limit decode bursts: never synchronously read every queued sound at once.
	for (int handled=0;r<w && handled<2;++r,++handled) {
		const auto request=requests[r%32]; const auto &group=pax_sound_groups[request.group-1];
		if(!group.count) continue;
		const auto gain=(request.flags&1)?pax_audio_gain(request.x,request.y,left,top,width,height):PaxAudioGain{255,255};
		if(!gain.left && !gain.right) continue;
		int channel=(request.flags&1)?-1:(request.flags&2)?1:0;
		if(channel<0) {
			for(int c=2;c<10;++c) if(voices[c].clip>=0 && voices[c].group==request.group) {channel=-2;break;}
			if(channel==-2) continue;
			for(int c=2;c<10;++c) if(voices[c].clip<0) {channel=c;break;}
			if(channel<0) {__atomic_fetch_add(&dropped,1,__ATOMIC_RELAXED);continue;}
		}
		unsigned offset=group.offset,count=group.count,variation=0;
		if(request.file<0) {
		variation=variants[request.group-1]++;
		if(request.flags&4) {
			const int64_t now=_pax_svc->get_time_us();
			const unsigned source=unsigned(request.flags)>>8;
			if(lastSelection!=request.group || selectionSource!=source || now-selectionTime>2000000) selectionCount=0;
			lastSelection=request.group;selectionSource=source;selectionTime=now;
			if(++selectionCount>3 && group.annoyed) {
				offset+=count;count=group.annoyed;variation=selectionCount-4;
				if(variation+1>=count) selectionCount=0;
			}
		} else if(!(request.flags&1)) {lastSelection=0;selectionCount=0;}
		requests[r%32].file=pax_sound_members[offset+variation%count];
		}
		const unsigned file=requests[r%32].file;
		const int slot=load(file);
		if(slot==-1 && asyncDecode) break;
		if(slot<0) continue;
		ramps[channel].replace();
		voices[channel]={slot,0,request.group,request.x,request.y,request.volume,request.flags,{}};++plays;
		if(!reported[file] && _pax_svc->log_printf) {reported[file]=true;_pax_svc->log_printf("Stargus: sound event=%s file=%s channel=%d\n",group.name,pax_sound_files[file],channel);}
	}
	__atomic_store_n(&readIndex,r,__ATOMIC_RELEASE);
	PaxAudioGain gain[10];
	for(int c=0;c<10;++c) gain[c]=(voices[c].flags&1)?pax_audio_gain(voices[c].x,voices[c].y,left,top,width,height):PaxAudioGain{255,255};
	for(int f=0;f<frames;++f) for(int side=0;side<2;++side) {
		int value=pcm[f*2+side];
		for(int c=0;c<10;++c) {
			auto &v=voices[c];if(v.clip<0) {value+=ramps[c].sample(0,side);continue;}const auto &clip=clips[v.clip];const int position=v.frame+f;
			const int at=position/clip.divisor;if(at>=clip.frames) {value+=ramps[c].sample(0,side);continue;}
			int sample=clip.pcm[at];if(clip.divisor==2 && (position&1) && at+1<clip.frames) sample=(sample+clip.pcm[at+1])/2;
			int edge=position<clip.frames*clip.divisor-position-1?position:clip.frames*clip.divisor-position-1;
			if(edge<110) sample=sample*edge/110;
			const int target=side?gain[c].right:gain[c].left,prior=side?v.prior.right:v.prior.left;
			const int ramp=f<110?f:110;
			value+=ramps[c].sample(sample*(prior+(target-prior)*ramp/110)/255*v.volume/(255*3),side);
		}
		pcm[f*2+side]=value<-32768?-32768:value>32767?32767:value;
	}
	for(int c=0;c<10;++c) {auto &v=voices[c];v.prior=gain[c];if(v.clip>=0) {v.frame+=frames;if(v.frame>=clips[v.clip].frames*clips[v.clip].divisor) v.clip=-1;}}
}
inline void close() {
	for(auto &v:voices) v={};for(unsigned i=0;i<24;++i) release(i);
	if(decodedClip.pcm) _pax_svc->mem_free(decodedClip.pcm);decodedClip={};jobState=0;asyncDecode=false;arena=nullptr;
}
}
