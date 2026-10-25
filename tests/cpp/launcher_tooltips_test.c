#include "lvgl.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void lv_mem_init(void){}
void lv_mem_deinit(void){}
void *lv_malloc_core(size_t n){return malloc(n);}
void lv_free_core(void *p){free(p);}
void *lv_realloc_core(void *p,size_t n){return realloc(p,n);}
lv_result_t lv_mem_test_core(void){return LV_RESULT_OK;}
void lv_mem_monitor_core(lv_mem_monitor_t *p){memset(p,0,sizeof(*p));}
#define TOOLBAR_BUTTON_COUNT 12
#define SETTINGS_FOCUS_COUNT 38
#define DETAIL_ACTION_COUNT 5
#define VISIBLE_ROWS 12
#define ROM_PICKER_VISIBLE_ROWS 6
#define COL_TEXT 0xffffff
#define COL_PANEL 0x102030
#define COL_ACCENT 0x00dddd
static bool g_tooltips_enabled=true,g_mouse_cursor_seen=true,g_keyboard_open,g_rom_picker_active,g_settings_mode;
static int g_rom_picker_selected,g_rom_picker_first,g_toolbar_focus=-1,g_settings_focus,g_detail_index=-1,g_detail_focus,g_detail_action_count;
static int g_canvas_w=320,g_canvas_h=240;
static lv_display_t *g_display;
static lv_obj_t *g_rom_picker_rows[6],*g_toolbar_buttons[12],*g_settings_focus_widgets[38],*g_detail_buttons[5],*g_buttons[12];
static lv_obj_t *g_settings_panel,*g_list,*g_detail_info_view;
static lv_obj_t *g_status_clock,*g_status_clock_icon,*g_status_network_icon,*g_status_battery_icon,*g_status_battery_level,*g_archive_s3_eye_buttons[2];
static struct {bool valid;} g_detail_extract_recipe;
static int ui_px(int x){return x;}
static const lv_font_t *ui_font(void){return &lv_font_montserrat_14;}
#include "tooltips.h"
static unsigned clicks;
static bool pressed;
static void read_pointer(lv_indev_t *input,lv_indev_data_t *data){(void)input;data->point.x=40;data->point.y=40;data->state=pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;}
static void clicked(lv_event_t *e){(void)e;clicks++;}
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *p){(void)a;(void)p;lv_display_flush_ready(d);}
static void pump(unsigned ms){for(unsigned i=0;i<ms;i+=20){lv_tick_inc(20);lv_timer_handler();tooltip_poll();}}
int main(void){
 lv_init();g_display=lv_display_create(320,240);
 static uint16_t buffer[320*20];lv_display_set_color_format(g_display,LV_COLOR_FORMAT_RGB565);
 lv_display_set_buffers(g_display,buffer,NULL,sizeof(buffer),LV_DISPLAY_RENDER_MODE_PARTIAL);lv_display_set_flush_cb(g_display,flush);
 lv_obj_t *button=lv_button_create(lv_screen_active());lv_obj_set_pos(button,25,25);lv_obj_set_size(button,70,40);lv_obj_update_layout(button);
 tooltip_bind(button,"A bounded help message that wraps to fit the display.");
 lv_obj_add_event_cb(button,clicked,LV_EVENT_CLICKED,NULL);
 lv_obj_send_event(button,LV_EVENT_HOVER_OVER,NULL);lv_tick_inc(649);tooltip_poll();assert(!g_tooltip_visible);
 lv_tick_inc(1);tooltip_poll();assert(g_tooltip_visible && !strcmp(lv_label_get_text(g_tooltip_label),tooltip_text_for(button)));
 lv_area_t bounds;lv_obj_get_coords(g_tooltip_label,&bounds);assert(bounds.x1>=0 && bounds.x2<320 && bounds.y1>=0 && bounds.y2<240);
 lv_obj_send_event(button,LV_EVENT_HOVER_LEAVE,NULL);assert(!g_tooltip_visible);
 g_toolbar_buttons[0]=button;g_toolbar_focus=0;tooltip_focus_current();pump(660);assert(g_tooltip_visible);
 g_tooltips_enabled=false;tooltip_poll();assert(!g_tooltip_visible && !g_tooltip_target);
 g_tooltips_enabled=true;
 lv_indev_t *input=lv_indev_create();lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(input,read_pointer);
 pressed=true;pump(900);assert(g_tooltip_visible);pressed=false;pump(100);assert(clicks==0);
 pressed=true;pump(80);pressed=false;pump(100);assert(clicks==1);
 g_tooltips_enabled=false;pressed=true;pump(900);pressed=false;pump(100);assert(clicks==2 && !g_tooltip_visible);g_tooltips_enabled=true;
 tooltip_queue(button,tooltip_text_for(button),true);tooltip_poll();assert(g_tooltip_visible);
 pump(6020);assert(!g_tooltip_visible);
 tooltip_queue(button,tooltip_text_for(button),false);lv_obj_delete(button);assert(!g_tooltip_target && !g_tooltip_focus);tooltip_poll();
 tooltip_reset();assert(!g_tooltip_label && !g_tooltip_count);
 lv_indev_delete(input);lv_display_delete(g_display);
 puts("Tooltip hover delay, focus, viewport, disable, expiry, deletion and hold-without-click passed");
}
