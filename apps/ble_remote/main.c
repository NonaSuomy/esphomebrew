// SPDX-License-Identifier: Apache-2.0
// Composite BLE remote. BLE state belongs to firmware; LVGL belongs to this PAX.
#define PAX_APP_SIDE 1
#include "psram_app.h"
#include "lvgl.h"
#define W 800
#define H 480
static const app_services_t *svc;
static lv_display_t *display;
static lv_indev_t *pointer;
static lv_obj_t *panels[4], *tabs_ui[4], *status_label, *drag_button;
static uint16_t *canvas;
static bool quitting, dragging;
static unsigned tab, modifiers;
static uint16_t game_buttons;
static uint8_t game_hat = 8;
static lv_point_t last;
static int source_w = W, source_h = H;
static uint32_t last_status, click_release_at;
static bool connected;
static lv_obj_t *host_buttons[10], *host_info, *forget_button;
static unsigned selected_host, host_count;
static bool forget_pending;
static uint32_t forget_started;
static const char *host_message="Select a slot, then Connect. Pair new uses an empty slot.";

typedef struct { int x,y,w,h; uint16_t bit; uint8_t direction; lv_obj_t *obj; } Control;
static Control controls[12];
static int remote(uint32_t cmd, int32_t a, int32_t b, const char *text) {
    return svc->ble_remote(cmd,a,b,text);
}
static void release_all(void) {
    remote(PAX_BLE_RELEASE,0,0,NULL);
    dragging=false; modifiers=0; game_buttons=0; game_hat=8; click_release_at=0;
    if(drag_button) lv_obj_remove_state(drag_button,LV_STATE_CHECKED);
    if(panels[0])for(uint32_t i=0;i<lv_obj_get_child_count(panels[0]);i++)
        lv_obj_remove_state(lv_obj_get_child(panels[0],i),LV_STATE_CHECKED);
    for(unsigned i=0;i<12;i++)if(controls[i].obj)lv_obj_remove_state(controls[i].obj,LV_STATE_CHECKED);
}
static uint32_t tick(void) { return svc->get_time_us()/1000; }
static void flush(lv_display_t *d,const lv_area_t *area,uint8_t *pixels) {
    (void)area;
    svc->display_write_frame_custom((uint16_t *)pixels,W,H,1.0f,false);
    lv_display_flush_ready(d);
}
static void read_touch(lv_indev_t *indev,lv_indev_data_t *data) {
    (void)indev;
    int x=0,y=0;
    if(svc->touch_read(&x,&y)) {
        data->point.x=x*W/source_w; data->point.y=y*H/source_h;
        data->state=LV_INDEV_STATE_PRESSED;
    } else data->state=LV_INDEV_STATE_RELEASED;
}
static lv_obj_t *label(lv_obj_t *p,const char *text,int x,int y) {
    lv_obj_t *o=lv_label_create(p); lv_label_set_text(o,text);lv_obj_set_pos(o,x,y);
    lv_obj_set_style_text_color(o,lv_color_hex(0xd9eaf4),0);return o;
}
static lv_obj_t *button(lv_obj_t *p,const char *text,int x,int y,int w,int h,
                        lv_event_cb_t cb,uintptr_t value,lv_event_code_t event) {
    lv_obj_t *o=lv_button_create(p);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);
    lv_obj_set_style_bg_color(o,lv_color_hex(0x235473),0);
    lv_obj_set_style_bg_color(o,lv_color_hex(0x138cb7),LV_STATE_CHECKED);
    lv_obj_t *t=lv_label_create(o);lv_label_set_text(t,text);lv_obj_center(t);
    if(cb)lv_obj_add_event_cb(o,cb,event,(void *)value);
    return o;
}
static void change_tab(lv_event_t *e) {
    release_all(); tab=(uintptr_t)lv_event_get_user_data(e);
    for(unsigned i=0;i<4;i++) {
        if(i==tab){lv_obj_remove_flag(panels[i],LV_OBJ_FLAG_HIDDEN);lv_obj_add_state(tabs_ui[i],LV_STATE_CHECKED);}
        else {lv_obj_add_flag(panels[i],LV_OBJ_FLAG_HIDDEN);lv_obj_remove_state(tabs_ui[i],LV_STATE_CHECKED);}
    }
}
static void close_app(lv_event_t *e) {(void)e;release_all();quitting=true;}
static void advertise(lv_event_t *e) {(void)e;remote(PAX_BLE_ADVERTISE,1,0,NULL);}
static void refresh_hosts(void) {
    if(!host_info)return;
    if(forget_pending && tick()-forget_started>6000)forget_pending=false;
    for(unsigned i=0;i<host_count;i++) {
        int flags=remote(PAX_BLE_HOST_STATE,i,0,NULL);
        const char *state=flags<0?"Starting":(flags&PAX_BLE_HOST_CONNECTED)?"Connected":(flags&PAX_BLE_HOST_SAVED)?"Saved":"Empty";
        lv_label_set_text_fmt(lv_obj_get_child(host_buttons[i],0),"%u: %s",i+1,state);
        if(i==selected_host)lv_obj_add_state(host_buttons[i],LV_STATE_CHECKED);
        else lv_obj_remove_state(host_buttons[i],LV_STATE_CHECKED);
    }
    int flags=remote(PAX_BLE_HOST_STATE,selected_host,0,NULL);
    if(flags<0){lv_label_set_text(host_info,"Bluetooth is starting. Saved hosts will appear shortly.");return;}
    lv_label_set_text_fmt(host_info,"Slot %u: %s%s\n%s",selected_host+1,(flags&PAX_BLE_HOST_SAVED)?"saved pairing":"empty",(flags&PAX_BLE_HOST_ACTIVE)?" (active)":"",host_message);
    lv_label_set_text(lv_obj_get_child(forget_button,0),forget_pending?"Confirm forget":"Forget selected");
}
static void select_host(lv_event_t *e) {
    selected_host=(uintptr_t)lv_event_get_user_data(e);forget_pending=false;
    host_message="Connect selects this host. Disconnect keeps its saved pairing.";refresh_hosts();
}
static void host_action(lv_event_t *e) {
    unsigned action=(uintptr_t)lv_event_get_user_data(e);
    if(action==3) {
        int flags=remote(PAX_BLE_HOST_STATE,selected_host,0,NULL);
        if(flags<0){forget_pending=false;host_message="Bluetooth is starting; try again shortly.";}
        else if(!(flags&PAX_BLE_HOST_SAVED)){forget_pending=false;host_message="This slot is already empty.";}
        else if(!forget_pending){forget_pending=true;forget_started=tick();host_message="Tap Confirm forget within 6 seconds to remove the saved pairing.";}
        else {release_all();remote(PAX_BLE_FORGET_HOST,selected_host,0,NULL);forget_pending=false;
            host_message="Forgotten. Also forget the NANO in the old host's Bluetooth settings.";}
    } else {
        forget_pending=false;
        if(action==0){release_all();int result=remote(PAX_BLE_SELECT_HOST,selected_host,0,NULL);
            host_message=result>=0?"Host enabled. For an empty slot, scan for PAX NANO BLE Test.":result==-6?"Bluetooth is starting; try again shortly.":"Host selection needs newer BLE Remote firmware.";}
        if(action==1){release_all();remote(PAX_BLE_ADVERTISE,0,0,NULL);host_message="Active host disconnected. Saved pairing kept; Connect resumes it.";}
        if(action==2){release_all();int result=remote(PAX_BLE_PAIR_NEW,0,0,NULL);
            if(result>=0){selected_host=result;host_message="Pair now: scan for PAX NANO BLE Test in Bluetooth settings.";}
            else host_message=result==-6?"Bluetooth is starting; try again shortly.":result==-5?"All slots are saved. Select a slot and Forget selected first.":"Pairing needs newer BLE Remote firmware.";}
    }
    refresh_hosts();
}
// HID keyboard usages for a US layout. Modifiers latch for the next key.
static void key(lv_event_t *e) {
    unsigned usage=(uintptr_t)lv_event_get_user_data(e);
    if(usage>=256) {
        unsigned mask=usage-256;
        modifiers^=mask;
        if(modifiers&mask)lv_obj_add_state(lv_event_get_target_obj(e),LV_STATE_CHECKED);
        else lv_obj_remove_state(lv_event_get_target_obj(e),LV_STATE_CHECKED);
        return;
    }
    remote(PAX_BLE_KEY,modifiers,usage,NULL);
    modifiers=0;
    uint32_t n=lv_obj_get_child_count(panels[0]);
    for(uint32_t i=0;i<n;i++)lv_obj_remove_state(lv_obj_get_child(panels[0],i),LV_STATE_CHECKED);
}
static int clamp(int x) {return x < -127 ? -127 : x > 127 ? 127 : x;}
static void mouse(lv_event_t *e) {
    lv_event_code_t code=lv_event_get_code(e);
    unsigned action=(uintptr_t)lv_event_get_user_data(e);
    if(action==10) {
        if(code!=LV_EVENT_PRESSED && code!=LV_EVENT_PRESSING)return;
        lv_indev_t *indev=lv_indev_active();if(!indev)return;
        lv_point_t point;lv_indev_get_point(indev,&point);
        if(code==LV_EVENT_PRESSED)last=point;
        if(code==LV_EVENT_PRESSING) {
            int dx=point.x-last.x,dy=point.y-last.y;last=point;
            if(dx||dy)remote(PAX_BLE_MOUSE_MOVE,clamp(dx),clamp(dy),NULL);
        }
    } else if(code==LV_EVENT_CLICKED) {
        if(action==3) {
            dragging=!dragging;
            remote(PAX_BLE_MOUSE_BUTTONS,dragging?1:0,0,NULL);
            if(dragging)lv_obj_add_state(drag_button,LV_STATE_CHECKED);
            else lv_obj_remove_state(drag_button,LV_STATE_CHECKED);
        } else if(action==4||action==5)remote(PAX_BLE_MOUSE_SCROLL,action==4?1:-1,0,NULL);
        else {
            remote(PAX_BLE_MOUSE_BUTTONS,action,0,NULL);
            // Give receivers that poll HID state time to observe a click.
            click_release_at=tick()+60;
        }
    }
}
// Poll all fingers, allowing simultaneous D-pad and action buttons.
static void poll_gamepad(void) {
    uint16_t bits=0; unsigned directions=0;
    pax_touch_point_t points[5];int n=0;
    if(svc->touch_read_points)n=svc->touch_read_points(points,5);
    // Single-point fallback also supports the loader's timed diagnostic taps.
    if(n<=0){int x,y;if(svc->touch_read(&x,&y)){points[0].x=x;points[0].y=y;n=1;}}
    for(unsigned i=0;i<12;i++) {
        Control *c=&controls[i]; bool down=false;
        for(int j=0;j<n && j<5;j++) {
            int x=points[j].x*W/source_w,y=points[j].y*H/source_h;
            if(x>=c->x && x<c->x+c->w && y>=c->y && y<c->y+c->h)down=true;
        }
        if(down){bits|=c->bit;directions|=c->direction;lv_obj_add_state(c->obj,LV_STATE_CHECKED);}
        else lv_obj_remove_state(c->obj,LV_STATE_CHECKED);
    }
    int x=!!(directions&2)-!!(directions&8),y=!!(directions&4)-!!(directions&1);
    uint8_t hat=y<0?(x<0?7:x>0?1:0):y>0?(x<0?5:x>0?3:4):x<0?6:x>0?2:8;
    if(bits!=game_buttons || hat!=game_hat){game_buttons=bits;game_hat=hat;remote(PAX_BLE_GAMEPAD,bits,hat,NULL);}
}
static void build_ui(void) {
    lv_obj_t *root=lv_screen_active();lv_obj_remove_flag(root,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root,lv_color_hex(0x081827),0);
    lv_obj_set_style_text_color(root,lv_color_white(),0);
    status_label=label(root,"BLE Remote - checking firmware",12,10);
    lv_obj_set_width(status_label,540);lv_label_set_long_mode(status_label,LV_LABEL_LONG_DOT);
    button(root,"Advertise",566,3,126,36,advertise,0,LV_EVENT_CLICKED);
    button(root,"Close",704,3,84,36,close_app,0,LV_EVENT_CLICKED);
    const char *tabs[]={"Keyboard","Mousepad","Gamepad","Hosts"};
    for(unsigned i=0;i<4;i++)tabs_ui[i]=button(root,tabs[i],12+i*196,48,186,42,change_tab,i,LV_EVENT_CLICKED);
    lv_obj_add_state(tabs_ui[0],LV_STATE_CHECKED);
    for(unsigned i=0;i<4;i++) {
        panels[i]=lv_obj_create(root);lv_obj_set_pos(panels[i],8,100);lv_obj_set_size(panels[i],784,372);
        lv_obj_set_style_pad_all(panels[i],0,0);lv_obj_set_style_border_width(panels[i],0,0);
        lv_obj_set_style_bg_color(panels[i],lv_color_hex(0x102b43),0);
        lv_obj_remove_flag(panels[i],LV_OBJ_FLAG_SCROLLABLE);
        if(i)lv_obj_add_flag(panels[i],LV_OBJ_FLAG_HIDDEN);
    }
    const char *rows[]={"1234567890","QWERTYUIOP","ASDFGHJKL","ZXCVBNM"};
    const unsigned rowcodes[4][10]={{30,31,32,33,34,35,36,37,38,39},{20,26,8,21,23,28,24,12,18,19},{4,22,7,9,10,11,13,14,15},{29,27,6,25,5,17,16}};
    for(unsigned r=0;r<4;r++)for(unsigned c=0;rows[r][c];c++) {
        char text[2]={rows[r][c],0};
        button(panels[0],text,8+c*69+(r>1?(r-1)*25:0),8+r*54,63,48,key,rowcodes[r][c],LV_EVENT_CLICKED);
    }
    button(panels[0],"Back",700,8,76,48,key,42,LV_EVENT_CLICKED);
    button(panels[0],"Tab",700,62,76,48,key,43,LV_EVENT_CLICKED);
    button(panels[0],"Enter",661,116,115,48,key,40,LV_EVENT_CLICKED);
    button(panels[0],"Esc",580,170,90,48,key,41,LV_EVENT_CLICKED);
    const char *mods[]={"Ctrl","Shift","Alt","Win"};const unsigned masks[]={1,2,4,8};
    for(unsigned i=0;i<4;i++)button(panels[0],mods[i],8+i*105,228,96,50,key,256+masks[i],LV_EVENT_CLICKED);
    button(panels[0],"Space",428,228,348,50,key,44,LV_EVENT_CLICKED);
    const char *extra[]={"- _","= +","[ {","] }","; :","' \"",", <",". >","/ ?","\\ |"};
    const unsigned codes[]={45,46,47,48,51,52,54,55,56,49};
    for(unsigned i=0;i<10;i++)button(panels[0],extra[i],8+i*77,288,70,38,key,codes[i],LV_EVENT_CLICKED);
    label(panels[0],"US layout. Modifiers apply to the next key. Shift gives capitals / symbols.",12,342);
    lv_obj_t *pad=lv_obj_create(panels[1]);lv_obj_set_pos(pad,8,8);lv_obj_set_size(pad,768,270);
    lv_obj_set_style_bg_color(pad,lv_color_hex(0x193d54),0);
    lv_obj_set_style_border_color(pad,lv_color_hex(0x32637e),0);
    lv_obj_remove_flag(pad,LV_OBJ_FLAG_SCROLLABLE);lv_obj_add_event_cb(pad,mouse,LV_EVENT_ALL,(void *)10);
    label(pad,"Slide to move pointer. Drag latches the left button until tapped again.",8,8);
    const char *mouse_names[]={"Left click","Right click","Drag","Scroll +","Scroll -"};
    for(unsigned i=0;i<5;i++) {
        lv_obj_t *b=button(panels[1],mouse_names[i],8+i*154,294,146,58,mouse,i+1,LV_EVENT_CLICKED);
        if(i==2)drag_button=b;
    }
    const char *names[]={"Up","Right","Down","Left","A","B","X","Y","L","R","Select","Start"};
    const unsigned bits[]={0,0,0,0,1,2,8,16,64,128,1024,2048};
    const int xy[12][2]={{96,40},{182,126},{96,212},{10,126},{390,70},{486,70},{582,70},{678,70},{390,180},{486,180},{582,180},{678,180}};
    label(panels[2],"Touch D-pad and buttons together. Standard BLE HID gamepad.",10,8);
    for(unsigned i=0;i<12;i++) {
        Control *c=&controls[i]; c->x=8+xy[i][0];c->y=100+xy[i][1];c->w=84;c->h=76;
        c->bit=bits[i];c->direction=i<4?(1u<<i):0;
        c->obj=button(panels[2],names[i],xy[i][0],xy[i][1],84,76,NULL,0,LV_EVENT_CLICKED);
    }
    label(panels[2],"16-button / 8-way hat report. Opposite directions cancel.",10,332);
    int count=remote(PAX_BLE_HOST_COUNT,0,0,NULL);
    host_count=count>0&&count<=10?(unsigned)count:0;
    int active=remote(PAX_BLE_ACTIVE_HOST,0,0,NULL);selected_host=active>=0&&active<(int)host_count?active:0;
    for(unsigned i=0;i<host_count;i++)host_buttons[i]=button(panels[3],"",8+(i%5)*154,8+(i/5)*60,146,52,select_host,i,LV_EVENT_CLICKED);
    host_info=label(panels[3],"",12,138);lv_obj_set_width(host_info,754);lv_label_set_long_mode(host_info,LV_LABEL_LONG_WRAP);
    const char *actions[]={"Connect selected","Disconnect","Pair new device","Forget selected"};
    for(unsigned i=0;i<4;i++){
        lv_obj_t *b=button(panels[3],actions[i],8+i*194,238,186,56,host_action,i,LV_EVENT_CLICKED);
        if(i==3)forget_button=b;
    }
    label(panels[3],"One active host. Saved pairings survive restarts.",12,322);
    if(!host_count)host_message="Update the NANO BLE Remote firmware to manage saved hosts.";
    refresh_hosts();
}
static int setup(void *arg) {
    (void)arg;lv_init();lv_tick_set_cb(tick);
    display=lv_display_create(W,H);if(!display)return -1;
    lv_display_set_default(display);lv_display_set_flush_cb(display,flush);
    lv_display_set_buffers(display,canvas,NULL,W*H*2,LV_DISPLAY_RENDER_MODE_FULL);
    pointer=lv_indev_create();if(!pointer)return -2;
    lv_indev_set_type(pointer,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(pointer,read_touch);lv_indev_set_display(pointer,display);
    build_ui();return 0;
}
static int pump(void *arg) {
    (void)arg;lv_timer_handler();
    if(click_release_at && (int32_t)(tick()-click_release_at)>=0) {
        remote(PAX_BLE_MOUSE_BUTTONS,dragging?1:0,0,NULL);click_release_at=0;
    }
    if(tick()-last_status>=500) {
        last_status=tick();int flags=remote(PAX_BLE_STATUS,0,0,NULL);
        bool now=flags>=0&&(flags&PAX_BLE_CONNECTED);
        if(connected&&!now)release_all();connected=now;
        int active=remote(PAX_BLE_ACTIVE_HOST,0,0,NULL);
        const char *state=now?"connected":flags<0?"BLE unavailable":!(flags&PAX_BLE_AVAILABLE)?"starting":(flags&PAX_BLE_ADVERTISING)?"advertising":"disconnected";
        lv_label_set_text_fmt(status_label,"BLE Remote | Host %u/%u | %s",active>=0?(unsigned)active+1:1,host_count,state);
        if(tab==3)refresh_hosts();
    }
    if(tab==2 && connected)poll_gamepad();
    return 0;
}
static int teardown(void *arg) {
    (void)arg;release_all();
    if(pointer){lv_indev_delete(pointer);pointer=NULL;}
    if(display){lv_display_delete(display);display=NULL;}
    lv_deinit();return 0;
}
__attribute__((section(".text.entry")))
int app_entry(const app_services_t *services) {
    svc=services;
    if(!svc || svc->abi_version!=PAX_ABI_VERSION || !svc->ble_remote || !svc->ui_call_sync || !svc->touch_read) {
        if(svc)svc->log_printf("BLE Remote requires firmware with the optional BLE remote service.\n");
        return -1;
    }
    if(svc->display_set_canvas && svc->display_set_canvas(W,H)!=0)return -2;
    if(svc->display_get_size)svc->display_get_size(&source_w,&source_h);
    if(source_w<=0||source_h<=0)return -2;
    canvas=svc->mem_caps_alloc(W*H*2,PAX_MEM_CAP_SPIRAM|PAX_MEM_CAP_DMA);
    if(!canvas)return -3;
    int result=svc->ui_call_sync(setup,NULL,2000);
    if(result==0)while(!quitting) {
        if(svc->input_l3_read && svc->input_l3_read())break;
        result=svc->ui_call_sync(pump,NULL,2000);if(result)break;
        svc->delay_ms(10);
    }
    int cleanup=svc->ui_call_sync(teardown,NULL,2000);
    // If the main loop cannot acknowledge teardown, leave buffers for loader cleanup.
    if(cleanup==0)svc->mem_free(canvas);
    return result?result:cleanup;
}
