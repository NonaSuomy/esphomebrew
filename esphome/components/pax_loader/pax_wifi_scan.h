#pragma once
#include <stdint.h>
#include <string.h>
#define PAX_WIFI_SCAN_FILE "/dev/pax-wifi-scan"
#define PAX_WIFI_SCAN_MAGIC 0x50575331U
#define PAX_WIFI_SCAN_MAX 24U
/* A separate virtual file keeps the existing numeric ABI and Wi-Fi status intact. */
typedef struct { uint32_t magic; } pax_wifi_scan_request_t;
typedef struct { char ssid[33]; int32_t rssi; uint32_t channel, secured; } pax_wifi_scan_ap_t;
typedef struct {
 uint32_t magic; int32_t state; uint32_t count, generation;
 char message[80]; pax_wifi_scan_ap_t aps[PAX_WIFI_SCAN_MAX];
} pax_wifi_scan_status_t;
static inline int pax_wifi_scan_valid(const pax_wifi_scan_status_t *s) {
 if(!s || s->magic!=PAX_WIFI_SCAN_MAGIC || s->state < -1 || s->state > 2 ||
    s->count>PAX_WIFI_SCAN_MAX || s->message[79])return 0;
 for(uint32_t i=0;i<s->count;i++)if(!s->aps[i].ssid[0] || s->aps[i].ssid[32] ||
    s->aps[i].secured>1 || s->aps[i].rssi < -127 || s->aps[i].rssi > 0)return 0;
 return 1;
}
/* Keep the strongest radio for each SSID/security pair and cap the result set.
 * The password field is intentionally absent from this protocol. */
static inline void pax_wifi_scan_add(pax_wifi_scan_status_t *s,const char *ssid,int rssi,
                                     uint32_t channel,uint32_t secured) {
 if(!ssid || !ssid[0] || strlen(ssid)>32 || rssi < -127 || rssi>0)return;
 uint32_t index=s->count;
 for(uint32_t i=0;i<s->count;i++)if(!strcmp(s->aps[i].ssid,ssid) && s->aps[i].secured==!!secured){
  if(s->aps[i].rssi>=rssi)return;
  index=i;break;
 }
 if(index==s->count){
  if(s->count<PAX_WIFI_SCAN_MAX)s->count++;
  else {index=s->count-1;if(s->aps[index].rssi>=rssi)return;}
 }
 pax_wifi_scan_ap_t value={{0},0,0,0};strncpy(value.ssid,ssid,32);value.rssi=rssi;value.channel=channel;value.secured=!!secured;
 s->aps[index]=value;
 for(uint32_t i=index;i>0 && s->aps[i].rssi>s->aps[i-1].rssi;i--){
  pax_wifi_scan_ap_t swap=s->aps[i-1];s->aps[i-1]=s->aps[i];s->aps[i]=swap;
 }
}
