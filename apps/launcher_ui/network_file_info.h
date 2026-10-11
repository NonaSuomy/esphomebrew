#ifndef PAX_NETWORK_FILE_INFO_H
#define PAX_NETWORK_FILE_INFO_H
/* Optional metadata attributes on LAN directory links. Never changes download paths. */
static bool pax_network_attribute(const char *tag,size_t length,const char *key,uint64_t *value)
{
 size_t key_length=strlen(key);
 for(size_t i=0;i+key_length+3<=length;i++){
  if(i && tag[i-1]!=' ' && tag[i-1]!='\t' && tag[i-1]!='\r' && tag[i-1]!='\n')continue;
  if(memcmp(tag+i,key,key_length)!=0)continue;
  size_t p=i+key_length;
  if(tag[p++]!='=')continue;
  char quote=tag[p++];if(quote!='"' && quote!='\'')continue;
  uint64_t number=0;size_t digits=0;
  while(p<length && tag[p]>='0' && tag[p]<='9'){
   unsigned digit=(unsigned)(tag[p++]-'0');if(number>(UINT64_MAX-digit)/10)return false;
   number=number*10+digit;digits++;
  }
  if(!digits || p>=length || tag[p]!=quote)return false;
  *value=number;return true;
 }
 return false;
}
static bool pax_network_file_info(const char *tag,size_t length,pax_file_stat_t *info)
{
 uint64_t size,mtime=0;
 if(!pax_network_attribute(tag,length,"data-pax-size",&size))return false;
 (void)pax_network_attribute(tag,length,"data-pax-mtime",&mtime);
 memset(info,0,sizeof(*info));info->size=size;
 info->mtime=mtime<=253402300799ULL ? (int64_t)mtime : 0;
 return true;
}
static bool pax_list_title_valid(const char *title)
{
 bool content=false;
 for(size_t i=0;title[i];i++){
  unsigned char c=(unsigned char)title[i];
  if(c<32 || c==127)return false;
  if(c!=' ')content=true;
 }
 return content;
}
/* UTF-8 title encoded with URL percent escapes, so quotes/entities cannot
 * change the HTML tag. Reject malformed escapes, controls and truncation. */
static int pax_title_hex(char c)
{
 if(c>='0' && c<='9')return c-'0';
 if(c>='a' && c<='f')return c-'a'+10;
 if(c>='A' && c<='F')return c-'A'+10;
 return -1;
}
static bool pax_network_title(const char *tag,size_t length,char *out,size_t capacity)
{
 const char *key="data-pax-title-url";size_t key_length=strlen(key);
 if(!capacity)return false;
 out[0]='\0';
 for(size_t i=0;i+key_length+3<=length;i++){
  if(i && tag[i-1]!=' ' && tag[i-1]!='\t' && tag[i-1]!='\r' && tag[i-1]!='\n')continue;
  if(memcmp(tag+i,key,key_length))continue;
  size_t p=i+key_length;if(tag[p++]!='=')continue;
  char quote=tag[p++];if(quote!='"' && quote!='\'')continue;
  size_t used=0;
  while(p<length && tag[p]!=quote){
   unsigned char c=(unsigned char)tag[p++];
   if(c=='%'){
    if(p+2>length)goto invalid;
    int hi=pax_title_hex(tag[p]),lo=pax_title_hex(tag[p+1]);
    if(hi<0 || lo<0)goto invalid;
    c=(unsigned char)((hi<<4)|lo);p+=2;
   }
   if(c<32 || c==127 || used+1>=capacity)goto invalid;
   out[used++]=(char)c;
  }
  if(p>=length || !used)goto invalid;
  out[used]='\0';if(!pax_list_title_valid(out))goto invalid;return true;
 }
invalid:
 out[0]='\0';return false;
}
#endif
