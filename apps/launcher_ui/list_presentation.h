#ifndef PAX_LIST_PRESENTATION_H
#define PAX_LIST_PRESENTATION_H
/* Display-only formatting: package names and paths remain authoritative. */
static void pax_info_append(char *out,size_t cap,const char *text)
{
 size_t n=strlen(out);while(*text && n+1<cap)out[n++]=*text++;if(cap)out[n]=0;
}
static void pax_info_number(char *out,size_t cap,uint64_t value,int width)
{
 char digits[24];int n=0;do{digits[n++]=(char)('0'+value%10);value/=10;}while(value);
 while(n<width)digits[n++]='0';while(n){char c[2]={digits[--n],0};pax_info_append(out,cap,c);}
}
static bool pax_info_leap(int year){return year%4==0 && (year%100!=0 || year%400==0);}
static void pax_info_date(char *out,size_t cap,int64_t stamp)
{
 if(stamp<=0 || stamp>253402300799LL){pax_info_append(out,cap,"Unavailable");return;}
 int64_t days=stamp/86400;int year=1970;
 while(days>=365+pax_info_leap(year)){days-=365+pax_info_leap(year);year++;}
 static const int months[12]={31,28,31,30,31,30,31,31,30,31,30,31};int month=0;
 while(month<11){int count=months[month]+(month==1 && pax_info_leap(year));if(days<count)break;days-=count;month++;}
 pax_info_number(out,cap,year,4);pax_info_append(out,cap,"-");pax_info_number(out,cap,month+1,2);
 pax_info_append(out,cap,"-");pax_info_number(out,cap,days+1,2);pax_info_append(out,cap," ");
 pax_info_number(out,cap,(stamp/3600)%24,2);pax_info_append(out,cap,":");pax_info_number(out,cap,(stamp/60)%60,2);pax_info_append(out,cap," UTC");
}
static void pax_list_label(char *out,size_t cap,const char *name,bool hide)
{
 if(!cap)return;out[0]=0;pax_info_append(out,cap,name);
 size_t n=strlen(out);
 if(hide && n>=4 && out[n-4]=='.' && (out[n-3]=='p'||out[n-3]=='P') &&
    (out[n-2]=='a'||out[n-2]=='A') && (out[n-1]=='x'||out[n-1]=='X'))out[n-4]=0;
}
static void pax_list_details(char *out,size_t cap,const pax_file_stat_t *info)
{
 if(!cap)return;out[0]=0;
 if(!info){pax_info_append(out,cap,"File details unavailable");return;}
 pax_info_number(out,cap,info->size,1);pax_info_append(out,cap," bytes | ");pax_info_date(out,cap,info->mtime);
}
#endif
