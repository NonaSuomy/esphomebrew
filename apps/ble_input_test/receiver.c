// SPDX-License-Identifier: Apache-2.0
// Temporary diagnostic PAX: only auto-connects to the named NANO test peripheral.
#define PAX_APP_SIDE 1
#include "psram_app.h"
#include "pax_bluetooth_settings.h"
#include "font.h"
#define W 512
#define H 300
void *memset(void *p,int c,unsigned n){unsigned char *b=p;while(n--)*b++=c;return p;}
void *memcpy(void *p,const void *q,unsigned n){unsigned char *a=p;const unsigned char *b=q;while(n--)*a++=*b++;return p;}
static int same_address(const uint8_t *a,const uint8_t *b){for(unsigned i=0;i<6;i++)if(a[i]!=b[i])return 0;return 1;}
static unsigned length(const char *s){unsigned n=0;while(s[n])n++;return n;}
static int target(const char *s){const char *name="PAX NANO BLE Test";for(unsigned i=0;i<17;i++)if(s[i]!=name[i])return 0;return 1;}
static char *append(char *p,const char *s){while(*s)*p++=*s++;*p=0;return p;}
static char *number(char *p,int n){unsigned v;if(n<0){*p++='-';v=0u-(unsigned)n;}else v=n;char b[12];unsigned k=0;do{b[k++]='0'+v%10;v/=10;}while(v);while(k)*p++=b[--k];*p=0;return p;}
static void rect(uint16_t *fb,int x,int y,int w,int h,uint16_t c){for(int j=y;j<y+h;j++)for(int i=x;i<x+w;i++)if(i>=0&&i<W&&j>=0&&j<H)fb[j*W+i]=c;}
static void text(uint16_t *fb,int x,int y,const char *s,uint16_t c){for(;*s;s++,x+=12){char ch=*s;if(ch>='a'&&ch<='z')ch-=32;unsigned k=0;while(font_chars[k]&&font_chars[k]!=ch)k++;if(!font_chars[k])continue;for(int col=0;col<5;col++)for(int row=0;row<7;row++)if(font_bits[k][col]&(1<<row))rect(fb,x+col*2,y+row*2,2,2,c);}}
static int command(const app_services_t *svc,unsigned op,const uint8_t *address){
 pax_bluetooth_request_t r={0};r.magic=PAX_BLUETOOTH_MAGIC;r.command.version=1;r.command.operation=op;r.command.ble=1;
 if(address)memcpy(r.command.address,address,6);
 void *f=svc->file_open(PAX_BLUETOOTH_FILE,"wb");if(!f)return -1;
 int ok=svc->file_write(&r,1,sizeof(r),f)==sizeof(r);int result=svc->file_close(f);return ok&&result==0?0:-1;
}
__attribute__((section(".text.entry"))) int app_entry(const app_services_t *svc){
 if(svc->abi_version!=PAX_ABI_VERSION||!svc->input_keyboard_read||!svc->input_mouse_read)return -1;
 unsigned run_id=(unsigned)svc->get_time_us();
 uint16_t *fb=svc->mem_caps_alloc(W*H*2,PAX_MEM_CAP_SPIRAM);if(!fb)return -1;
 char peer_name[49]={0};
 char typed[65]={0};unsigned used=0;int downs=0,ups=0,held=0,dx_total=0,dy_total=0,mouse_events=0,clicks=0,buttons=0,last_key=0,connected=0,seen=0;
 unsigned char keys[512]={0};int64_t poll=0,save=0,attempt=-20000000;int report_dirty=1;
 unsigned pad_mask=0,pad_changes=0,debug_snapshots=0;
 uint8_t tracked[6]={0};int have_target=0;
 pax_bluetooth_status_t status={0};int width=1024,height=600;if(svc->display_get_size)svc->display_get_size(&width,&height);
 float scale=(float)width/W;if((float)height/H<scale)scale=(float)height/H;
 svc->log_printf("BLE_TEST receiver ready; target=PAX NANO BLE Test\n");
 for(;;){
  pax_gamepad_state_t pad={0};svc->input_gamepad_read(&pad);if(pad.values[PAX_INPUT_MENU]&&pad.values[PAX_INPUT_X])break;
  unsigned mask=0;for(unsigned i=0;i<PAX_INPUT_MAX;i++)if(pad.values[i])mask|=1u<<i;
  if(mask!=pad_mask){pad_mask=mask;pad_changes++;report_dirty=1;svc->log_printf("BLE_TEST gamepad mask=%u changes=%u\n",mask,pad_changes);}
  int64_t now=svc->get_time_us();
  if(now-poll>=1000000){int was_connected=connected;poll=now;void *f=svc->file_open(PAX_BLUETOOTH_FILE,"rb");if(f){svc->file_read(&status,1,sizeof(status),f);svc->file_close(f);if(debug_snapshots++<12){void *out=svc->file_open("/sd/roms/pax/ble_status.bin","wb");if(out){svc->file_write(&status,1,sizeof(status),out);svc->file_close(out);}}}
   connected=0;seen=0;unsigned unnamed_connected=0;
   for(unsigned i=0;i<status.count&&i<16;i++){
    if(status.devices[i].ble&&status.devices[i].hid&&status.devices[i].connected&&!status.devices[i].name[0]){unnamed_connected++;}
    if(target(status.devices[i].name)){memcpy(tracked,status.devices[i].address,6);have_target=1;}
    if(!(target(status.devices[i].name)||(have_target&&same_address(tracked,status.devices[i].address))))continue;
    seen=1;connected=status.devices[i].connected;
    for(unsigned n=0;n<48;n++)if(peer_name[n]!=status.devices[i].name[n]){report_dirty=1;break;}
    memcpy(peer_name,status.devices[i].name,48);peer_name[48]=0;
    for(unsigned n=0;peer_name[n];n++)if(peer_name[n]=='"'||peer_name[n]=='\\'||(unsigned char)peer_name[n]<32)peer_name[n]='_';
    if(!connected&&!status.busy&&now-attempt>=5000000){int result=command(svc,PAX_BT_CONNECT,status.devices[i].address);svc->log_printf("BLE_TEST connect result=%d\n",result);attempt=now;}
   }
   // The current receiver can replace a cached name with an empty GATT name.
   // Observe a single already-connected unnamed HID peer; never auto-pair it.
   if(!seen&&unnamed_connected==1){seen=1;connected=1;}
   if(was_connected!=connected)report_dirty=1;
   if(!seen&&status.available&&!status.busy&&now-attempt>=20000000){int result=command(svc,PAX_BT_SCAN,0);svc->log_printf("BLE_TEST scan result=%d\n",result);attempt=now;}
  }
  pax_keyboard_event_t e;for(unsigned budget=0;budget<32&&svc->input_keyboard_read(&e);budget++){
   report_dirty=1;last_key=e.key;if(e.down){downs++;if(e.key>=32&&e.key<127&&e.key!='"'&&e.key!='\\'&&used<64){typed[used++]=e.key;typed[used]=0;}}
   else ups++;
   if(e.key>=0&&e.key<512&&keys[e.key]!=(e.down!=0)){keys[e.key]=(e.down!=0);held+=e.down?1:-1;}
   svc->log_printf("BLE_TEST key=%d down=%d held=%d text=%s\n",e.key,e.down,held,typed);
  }
  int dx=0,dy=0,b=0;svc->input_mouse_read(&dx,&dy,&b);
  if(dx||dy||b!=buttons){report_dirty=1;dx_total+=dx;dy_total+=dy;mouse_events++;if((b&1)&&!(buttons&1))clicks++;buttons=b;svc->log_printf("BLE_TEST mouse dx=%d dy=%d buttons=%d total=%d,%d\n",dx,dy,b,dx_total,dy_total);}
  if(report_dirty&&now-save>=250000){save=now;report_dirty=0;char report[512],*p=append(report,"{\"run\":");p=number(p,run_id);p=append(p,",\"connected\":");p=number(p,connected);p=append(p,",\"peer\":\"");p=append(p,peer_name);p=append(p,"\",\"text\":\"");p=append(p,typed);p=append(p,"\",\"downs\":");p=number(p,downs);p=append(p,",\"ups\":");p=number(p,ups);p=append(p,",\"held\":");p=number(p,held);p=append(p,",\"dx\":");p=number(p,dx_total);p=append(p,",\"dy\":");p=number(p,dy_total);p=append(p,",\"mouse_events\":");p=number(p,mouse_events);p=append(p,",\"clicks\":");p=number(p,clicks);p=append(p,",\"buttons\":");p=number(p,buttons);p=append(p,",\"gamepad\":");p=number(p,pad_mask);p=append(p,",\"gamepad_changes\":");p=number(p,pad_changes);append(p,"}\n");
   void *f=svc->file_open("/sd/roms/pax/ble_input_test.json","wb");if(f){svc->file_write(report,1,length(report),f);svc->file_close(f);}
  }
  rect(fb,0,0,W,H,0x1082);text(fb,20,18,"PAX BLE INPUT TEST",0xffff);
  text(fb,20,52,connected?"BLE HID CONNECTED":seen?"NANO FOUND - CONNECTING":"SEARCHING FOR NANO",connected?0x07e0:0xffe0);
  text(fb,20,88,"TEXT:",0xffff);text(fb,20,110,typed,0x07ff);
  char line[80],*p=append(line,"DOWN ");p=number(p,downs);p=append(p,"  UP ");p=number(p,ups);p=append(p,"  HELD ");number(p,held);text(fb,20,152,line,0xffff);
  p=append(line,"MOUSE ");p=number(p,dx_total);p=append(p,",");p=number(p,dy_total);p=append(p," CLICKS ");number(p,clicks);text(fb,20,180,line,0xffff);
  p=append(line,"LAST KEY ");number(p,last_key);text(fb,20,208,line,0xbdf7);
  p=append(line,"GAMEPAD ");p=number(p,pad_mask);p=append(p," CHANGES ");number(p,pad_changes);text(fb,20,236,line,0xffff);
  text(fb,20,274,"CLOSE WITH DEVICE REMOTE",0xbdf7);
  svc->display_write_frame_custom(fb,W,H,scale,0);svc->delay_ms(25);
 }
 svc->mem_free(fb);return 0;
}
