#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

enum { PAX_THEME_BACKGROUND,PAX_THEME_PANEL,PAX_THEME_SELECTED,PAX_THEME_ACCENT,PAX_THEME_TEXT,PAX_THEME_MUTED,PAX_THEME_OK,PAX_THEME_WARN,PAX_THEME_ERROR,PAX_THEME_COLORS };
typedef struct { char name[32];uint32_t colors[PAX_THEME_COLORS]; } pax_theme_profile_t;
#define PAX_THEME_FILE "/sd/roms/pax/.launcher_ui_theme.json"
#define PAX_THEME_FOLDER "/sd/roms/pax/themes"
#define PAX_THEME_PRESETS 5
static const char *const pax_theme_keys[PAX_THEME_COLORS]={"background","panel","selected","accent","text","muted","ok","warn","error"};
static const pax_theme_profile_t pax_theme_presets[PAX_THEME_PRESETS]={
 {"PAX",{0x071525,0x102844,0x15699D,0x25C7FF,0xF2F7FF,0x8BA7C4,0x4ADE80,0xFBBF24,0xF87171}},
 {"Emerald",{0x071812,0x123529,0x1F6852,0x4AE0AA,0xF0FFF7,0x91B9AA,0x75ED75,0xFFD166,0xFF7B86}},
 {"Amber",{0x191207,0x352714,0x694817,0xFFBF47,0xFFF6E5,0xC1A780,0x8ADC80,0xFFE078,0xFF8070}},
 {"Violet",{0x130D24,0x291C47,0x513785,0xB598FF,0xFAF5FF,0xAC9BC6,0x65DEAF,0xFFD166,0xFF859B}},
 {"Cyberpunk",{0x090615,0x18112B,0x4F1E68,0xFF2BD6,0xF7EDFF,0x9D8BB5,0x00F0FF,0xFFE600,0xFF4A6E}}
};
static inline bool pax_theme_string(const char *json,const char *key,char *out,size_t cap){
 char quoted[48];size_t n=strlen(key);if(n+3>sizeof(quoted))return false;
 quoted[0]='"';memcpy(quoted+1,key,n);quoted[n+1]='"';quoted[n+2]=0;
 const char *p=strstr(json,quoted);if(!p)return false;p+=n+2;
 while(*p==' '||*p=='\n'||*p=='\r'||*p=='\t') { p++; }
 if(*p++!=':')return false;
 while(*p==' '||*p=='\n'||*p=='\r'||*p=='\t') { p++; }
 if(*p++!='"')return false;
 n=0;
 while(*p && *p!='"'){if((unsigned char)*p<32 || (unsigned char)*p>126 || *p=='\\' || n+1>=cap)return false;out[n++]=*p++;}
 out[n]=0;return *p=='"';
}
static inline bool pax_theme_parse(const char *json,pax_theme_profile_t *out){
 size_t length=strlen(json);if(length<2||length>2048)return false;
 const char *first=json;while(*first==' '||*first=='\n'||*first=='\r'||*first=='\t')first++;if(*first!='{')return false;
 size_t end=length;while(end && (json[end-1]==' '||json[end-1]=='\n'||json[end-1]=='\r'||json[end-1]=='\t'))end--;if(!end||json[end-1]!='}')return false;
 pax_theme_profile_t result;memset(&result,0,sizeof(result));if(!pax_theme_string(json,"name",result.name,sizeof(result.name))||!result.name[0])return false;
 for(int i=0;i<PAX_THEME_COLORS;i++){char value[8];if(!pax_theme_string(json,pax_theme_keys[i],value,sizeof(value))||strlen(value)!=7||value[0]!='#')return false;
  uint32_t color=0;for(int j=1;j<7;j++){char c=value[j];int digit=c>='0'&&c<='9'?c-'0':c>='A'&&c<='F'?c-'A'+10:c>='a'&&c<='f'?c-'a'+10:-1;if(digit<0)return false;color=(color<<4)|(unsigned)digit;}result.colors[i]=color;
 }*out=result;return true;
}
static inline bool pax_theme_serialize(const pax_theme_profile_t *theme,char *out,size_t cap){
 if(cap<640)return false;
 char *p=out;const char *prefix="{\n  \"name\": \"";while(*prefix)*p++=*prefix++;
 for(const char *s=theme->name;*s;s++){if(*s=='"'||*s=='\\'||(unsigned char)*s<32)return false;*p++=*s;}
 const char *suffix="\",\n";while(*suffix)*p++=*suffix++;
 for(int i=0;i<PAX_THEME_COLORS;i++){*p++=' ';*p++=' ';*p++='"';for(const char *s=pax_theme_keys[i];*s;s++)*p++=*s;
  const char *sep="\": \"#";while(*sep)*p++=*sep++;for(int shift=20;shift>=0;shift-=4)*p++="0123456789ABCDEF"[(theme->colors[i]>>shift)&15];*p++='"';if(i+1<PAX_THEME_COLORS)*p++=',';*p++='\n';
 }*p++='}';*p++='\n';*p=0;return true;
}
