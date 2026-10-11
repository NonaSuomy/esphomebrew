#pragma once
#include <stdint.h>
#include <string.h>
/* Virtual FILE endpoint: uses existing ABI-1 file services, never SD storage. */
#define PAX_WIFI_SETTINGS_FILE "/dev/pax-wifi"
#define PAX_WIFI_SETTINGS_MAGIC 0x50574631U
typedef struct { uint32_t magic, replace_password; char ssid[33], password[65]; } pax_wifi_request_t;
typedef struct { uint32_t magic; int32_t status; uint32_t password_set; char ssid[33]; } pax_wifi_status_t;
static inline int pax_wifi_terminated(const char *s,size_t n){for(size_t i=0;i<n;i++)if(!s[i])return 1;return 0;}
static inline void pax_wifi_wipe(void *ptr,size_t n){volatile unsigned char *p=(volatile unsigned char *)ptr;while(n--)*p++=0;}
static inline int pax_wifi_request_valid(const pax_wifi_request_t *r) {
 if (!r || r->magic!=PAX_WIFI_SETTINGS_MAGIC || r->replace_password>1 ||
     !pax_wifi_terminated(r->ssid,sizeof(r->ssid)) || !r->ssid[0] ||
     !pax_wifi_terminated(r->password,sizeof(r->password))) return 0;
 if (!r->replace_password) return 1;
 size_t n=strlen(r->password);
 if (n==0 || (n>=8 && n<=63)) return 1;
 if (n!=64) return 0;
 for(size_t i=0;i<n;i++){char ch=r->password[i];if(!((ch>='0'&&ch<='9')||(ch>='a'&&ch<='f')||(ch>='A'&&ch<='F')))return 0;}
 return 1;
}
