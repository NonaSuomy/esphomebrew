/* Exercise the real inline HTTP stream against a server that ignores Range. */
#define PAX_NETSURF_INLINE_MEDIA 1
#include "../../video/pax_io.c"
#include <assert.h>
#include <stdarg.h>
#define BODY_SIZE 700000
static const char header[] = "HTTP/1.1 200 OK\r\nContent-Length: 700000\r\n\r\n";
static size_t cursor;
static int connections;
static int connect_fake(uint32_t ip, uint16_t port) { (void)ip; (void)port; cursor=0; ++connections; return 1; }
static int poll_fake(int h) { (void)h; return 2; }
static int resolve_fake(const char *host, uint32_t *ip) { (void)host; *ip=0; return 1; }
static int send_fake(int h, const void *data, int n) { (void)h; (void)data; return n; }
static int recv_fake(int h, void *data, int n) {
    (void)h; unsigned char *out=data; int count=0;
    while (count<n && cursor<sizeof(header)-1+BODY_SIZE) {
        out[count++]=cursor<sizeof(header)-1 ? (unsigned char)header[cursor] : (unsigned char)((cursor-(sizeof(header)-1))%251);
        ++cursor;
    }
    return count;
}
static void close_fake(int h) { (void)h; }
static app_services_t services = {.net_tcp_connect=connect_fake,.net_poll=poll_fake,.net_resolve=resolve_fake,
    .net_tcp_send=send_fake,.net_tcp_recv=recv_fake,.net_udp_close=close_fake};
const app_services_t *pax_svc=&services;
int64_t pax_time_us(void) { static int64_t now; return ++now; }
void pax_sleep_ms(int ms) { (void)ms; }
void pax_yield_if_due(void) {}
void pax_log(const char *fmt, ...) { (void)fmt; }
void *pax_alloc_aligned(size_t size, size_t align) { void *p=NULL; return posix_memalign(&p,align,size) ? NULL : p; }
int main(void) {
    char err[160]; volatile int stop=0; unsigned char data[8192];
    pax_stream_t *s=pax_stream_open("http://127.0.0.1/video.mp4",&stop,err,sizeof(err));
    assert(s && pax_stream_seekable(s));
    for (int at=0; at<BODY_SIZE;) {
        int n=pax_stream_read(s,data,sizeof(data)); assert(n>0);
        for (int i=0;i<n;i++) assert(data[i]==(unsigned char)((at+i)%251));
        at+=n;
    }
    assert(pax_stream_seek(s,0)==0);
    assert(pax_stream_read(s,data,sizeof(data))==(int)sizeof(data));
    for (int i=0;i<(int)sizeof(data);i++) assert(data[i]==(unsigned char)(i%251));
    assert(connections>=2);
    assert(pax_stream_seek(s,600000)==0);
    assert(pax_stream_read(s,data,sizeof(data))==(int)sizeof(data));
    for (int i=0;i<(int)sizeof(data);i++) assert(data[i]==(unsigned char)((600000+i)%251));
    pax_stream_close(s); puts("HTTP rewind without Range passed");
}
