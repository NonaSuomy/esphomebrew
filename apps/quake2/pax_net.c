#include "qcommon/qcommon.h"
#include <stdio.h>
#include <string.h>

typedef struct { byte data[MAX_MSGLEN]; int len; } loopmsg_t;
static loopmsg_t q[2][4];
static int getp[2], putp[2];
netadr_t net_local_adr;

void NET_Init(void) { memset(q, 0, sizeof q); net_local_adr.type = NA_LOOPBACK; }
void NET_Shutdown(void) {}
void NET_Config(qboolean multiplayer) { (void)multiplayer; }
qboolean NET_CompareAdr(netadr_t a, netadr_t b) { return a.type == b.type && a.port == b.port && !memcmp(a.ip,b.ip,4); }
qboolean NET_CompareBaseAdr(netadr_t a, netadr_t b) { return a.type == b.type && !memcmp(a.ip,b.ip,4); }
qboolean NET_IsLocalAddress(netadr_t a) { return a.type == NA_LOOPBACK; }
char *NET_AdrToString(netadr_t a) { static char s[32]; snprintf(s,sizeof s,"%d.%d.%d.%d:%d",a.ip[0],a.ip[1],a.ip[2],a.ip[3],a.port); return s; }
qboolean NET_StringToAdr(char *s, netadr_t *a) { (void)s; a->type = NA_LOOPBACK; memset(a->ip,0,4); a->port = 0; return true; }
void NET_Sleep(int msec) { (void)msec; }
qboolean NET_GetPacket(netsrc_t sock, netadr_t *from, sizebuf_t *msg) {
    int i = (int)sock; if (getp[i] == putp[i]) return false;
    loopmsg_t *m = &q[i][getp[i]]; if (m->len > msg->maxsize) m->len = msg->maxsize;
    memcpy(msg->data, m->data, m->len); msg->cursize = m->len; getp[i] = (getp[i]+1)&3;
    from->type = NA_LOOPBACK; return true;
}
void NET_SendPacket(netsrc_t sock, int len, void *data, netadr_t to) {
    (void)to; int i = 1 - (int)sock; int n = putp[i]; int next = (n+1)&3; if (next == getp[i]) return;
    if (len > MAX_MSGLEN) len = MAX_MSGLEN; memcpy(q[i][n].data,data,len); q[i][n].len=len; putp[i]=next;
}
