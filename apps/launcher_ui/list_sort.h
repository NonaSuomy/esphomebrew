#ifndef PAX_LIST_SORT_H
#define PAX_LIST_SORT_H
/* Sort view indices only; package identities and Archive records stay intact. */
enum {PAX_SORT_SOURCE, PAX_SORT_NAME_ASC, PAX_SORT_NAME_DESC, PAX_SORT_NEWEST,
      PAX_SORT_OLDEST, PAX_SORT_LARGEST, PAX_SORT_SMALLEST, PAX_SORT_COUNT};
static const char *pax_sort_name(int mode)
{
 static const char *names[]={"Source order","Name A-Z","Name Z-A","Newest first","Oldest first","Largest first","Smallest first"};
 return names[mode>=0 && mode<PAX_SORT_COUNT ? mode : 0];
}
static int pax_sort_text(const char *a,const char *b)
{
 while(*a && *b){
  unsigned char x=(unsigned char)*a++,y=(unsigned char)*b++;
  if(x>='A' && x<='Z')x+=32;if(y>='A' && y<='Z')y+=32;
  if(x!=y)return x<y ? -1 : 1;
 }
 return *a ? 1 : *b ? -1 : 0;
}
static int pax_sort_compare(int mode,const char *a,const char *ap,const pax_file_stat_t *as,
                           const char *b,const char *bp,const pax_file_stat_t *bs)
{
 if(mode==PAX_SORT_SOURCE)return 0;
 bool dates=mode==PAX_SORT_NEWEST || mode==PAX_SORT_OLDEST;
 bool sizes=mode==PAX_SORT_LARGEST || mode==PAX_SORT_SMALLEST;
 if(dates || sizes){
  bool av=as && (!dates || (as->mtime>0 && as->mtime<=253402300799LL));
  bool bv=bs && (!dates || (bs->mtime>0 && bs->mtime<=253402300799LL));
  if(av!=bv)return av ? -1 : 1; /* Unknown metadata always goes last. */
  if(av){
   uint64_t x=dates ? (uint64_t)as->mtime : as->size;
   uint64_t y=dates ? (uint64_t)bs->mtime : bs->size;
   if(x!=y){int c=x<y ? -1 : 1;return mode==PAX_SORT_NEWEST || mode==PAX_SORT_LARGEST ? -c : c;}
  }
 }
 int c=pax_sort_text(a,b);
 if(mode==PAX_SORT_NAME_DESC)c=-c;
 return c ? c : strcmp(ap,bp);
}
#endif
