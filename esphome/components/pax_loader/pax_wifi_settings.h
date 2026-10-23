#pragma once
#include <stdint.h>
#include <string.h>
/* Virtual FILE endpoint: uses existing ABI-1 file services, never SD storage. */
#define PAX_WIFI_SETTINGS_FILE "/dev/pax-wifi"
#define PAX_WIFI_SETTINGS_MAGIC 0x50574631U
typedef struct { uint32_t magic, replace_password; char ssid[33], password[65]; } pax_wifi_request_t;
/* The first 48 bytes retain the original status layout for older launchers. */
#define PAX_WIFI_STATUS_BASE_BYTES 48U
typedef struct { uint32_t magic; int32_t status; uint32_t password_set; char ssid[33]; uint32_t channel, frequency_mhz; char security[48]; } pax_wifi_status_t;
static inline uint32_t pax_wifi_channel_mhz(uint32_t channel) {
 if(channel>=1 && channel<=13)return 2407+channel*5;
 if(channel==14)return 2484;
 if(channel>=36 && channel<=177)return 5000+channel*5;
 return 0;
}
static inline const char *pax_wifi_auth_name(uint32_t mode) {
 switch(mode){case 0:return "Open";case 1:return "WEP";case 2:return "WPA-PSK";case 3:return "WPA2-PSK";case 4:return "WPA/WPA2-PSK";case 5:return "WPA2-Enterprise";case 6:return "WPA3-SAE";case 7:return "WPA2/WPA3-PSK";case 8:return "WAPI-PSK";case 9:return "OWE";case 10:return "WPA3-Enterprise-192";case 13:return "DPP";case 14:return "WPA3-Enterprise";case 15:return "WPA2/WPA3-Enterprise";case 16:return "WPA-Enterprise";default:return "Unknown security";}
}
static inline const char *pax_wifi_cipher_name(uint32_t cipher) {
 switch(cipher){case 0:return "";case 1:return "WEP40";case 2:return "WEP104";case 3:return "TKIP";case 4:return "AES-CCMP";case 5:return "TKIP/AES";case 6:return "AES-CMAC";case 7:return "SMS4";case 8:return "AES-GCMP";case 9:return "AES-GCMP256";case 10:return "AES-GMAC";case 11:return "AES-GMAC256";default:return "Unknown cipher";}
}
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
