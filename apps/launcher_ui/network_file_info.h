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
#endif
