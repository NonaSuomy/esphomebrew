#define _POSIX_C_SOURCE 200809L
#include "../audio_buffer.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdatomic.h>
static atomic_uint nonzero, frames, sample_rate;
static int64_t now(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000; }
static void delay(int ms) { struct timespec t={ms/1000,(ms%1000)*1000000L};nanosleep(&t,0); }
static void init(int rate) { atomic_store(&sample_rate,rate); }
static void submit(short *p,int n) { for(int i=0;i<n*2;i++) if(p[i]) atomic_fetch_add(&nonzero,1);atomic_fetch_add(&frames,n); }
typedef struct { pthread_t thread;void (*fn)(void *);void *arg; } task_t;
static void *entry(void *arg) {task_t *t=arg;t->fn(t->arg);return 0;}
static int create(void (*fn)(void *),const char *name,uint32_t depth,void *arg,int priority,void *out,int core) {
 (void)name;(void)depth;(void)priority;(void)core;task_t *t=malloc(sizeof *t);assert(t);t->fn=fn;t->arg=arg;
 if(pthread_create(&t->thread,0,entry,t)) {free(t);return -1;}*(void **)out=t;return 0;
}
static void destroy(void *h) {task_t *t=h;pthread_cancel(t->thread);pthread_join(t->thread,0);free(t);}
int main(void) {
 app_services_t s={0};s.mem_alloc=malloc;s.mem_free=free;s.get_time_us=now;s.delay_ms=delay;s.audio_init=init;s.audio_submit=submit;s.task_create=create;s.task_delete=destroy;
 audio_buffer_t a;assert(!audio_buffer_open(&a,&s));
 uint8_t *pcm=malloc(16000*2);assert(pcm);for(int i=0;i<16000;i++){pcm[2*i]=0x34;pcm[2*i+1]=0x12;}
 assert(!audio_buffer_write(&a,pcm,16000*2,1,16000,0,0));
 delay(50);audio_buffer_pause(&a,1);unsigned read=__atomic_load_n(&a.read_seq,__ATOMIC_ACQUIRE);delay(40);unsigned read2=__atomic_load_n(&a.read_seq,__ATOMIC_ACQUIRE);assert(read2-read<=1);
 audio_buffer_pause(&a,0);audio_buffer_finish(&a,1);
 assert(atomic_load(&sample_rate)==16000);assert(atomic_load(&nonzero)==16000*2);
 audio_buffer_mute(&a,1);assert(!audio_buffer_write(&a,pcm,16000*2,1,16000,0,0));audio_buffer_finish(&a,1);assert(atomic_load(&nonzero)==32000);
 audio_buffer_close(&a);assert(!a.blocks && !a.task);free(pcm);
 puts("Audio worker: mono conversion, paced drain, pause, mute and task cleanup passed");
}
