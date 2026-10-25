#pragma once
/* UI-task-only help. No filesystem work or waits run in an LVGL callback. */
typedef struct { lv_obj_t *widget; const char *text; } TooltipBinding;
static TooltipBinding g_tooltip_bindings[96];
static unsigned g_tooltip_count;
static lv_obj_t *g_tooltip_label,*g_tooltip_target,*g_tooltip_focus;
static const char *g_tooltip_text;
static uint32_t g_tooltip_since;
static bool g_tooltip_visible;

static void tooltip_hide(void)
{
  if(g_tooltip_label)lv_obj_add_flag(g_tooltip_label,LV_OBJ_FLAG_HIDDEN);
  g_tooltip_target=NULL;g_tooltip_text=NULL;g_tooltip_visible=false;
}

static void tooltip_reset(void)
{
  /* The display owns the label; drop all pointers before destroying it. */
  tooltip_hide();g_tooltip_label=NULL;g_tooltip_focus=NULL;g_tooltip_count=0;
}

static const char *tooltip_text_for(lv_obj_t *widget)
{
  for(unsigned i=0;i<g_tooltip_count;i++)
    if(g_tooltip_bindings[i].widget==widget)return g_tooltip_bindings[i].text;
  return NULL;
}

static void tooltip_queue(lv_obj_t *widget,const char *text,bool immediate)
{
  tooltip_hide();
  if(!g_tooltips_enabled || !widget || !text)return;
  g_tooltip_target=widget;g_tooltip_text=text;
  g_tooltip_since=lv_tick_get()-(immediate ? 650U : 0U);
}

static void tooltip_event(lv_event_t *event)
{
  lv_obj_t *widget=lv_event_get_current_target(event);
  lv_event_code_t code=lv_event_get_code(event);
  if(code==LV_EVENT_DELETE){
    if(g_tooltip_target==widget)tooltip_hide();
    if(g_tooltip_focus==widget)g_tooltip_focus=NULL;
    for(unsigned i=0;i<g_tooltip_count;i++)if(g_tooltip_bindings[i].widget==widget)g_tooltip_bindings[i].widget=NULL;
  }else if(code==LV_EVENT_HOVER_OVER){
    if(g_mouse_cursor_seen)tooltip_queue(widget,(const char *)lv_event_get_user_data(event),false);
  }else if(code==LV_EVENT_LONG_PRESSED){
    if(!g_tooltips_enabled)return;
    tooltip_queue(widget,(const char *)lv_event_get_user_data(event),true);
    /* Holding for help must never activate an install/eject/update on release. */
    lv_indev_t *input=lv_indev_active();if(input)lv_indev_wait_release(input);
  }else if(code==LV_EVENT_SCROLL_BEGIN || code==LV_EVENT_PRESSED ||
           (code==LV_EVENT_HOVER_LEAVE && g_tooltip_target==widget))tooltip_hide();
}

static void tooltip_bind(lv_obj_t *widget,const char *text)
{
  if(!widget || !text || g_tooltip_count>=96)return;
  lv_obj_add_flag(widget,LV_OBJ_FLAG_CLICKABLE);
  g_tooltip_bindings[g_tooltip_count++]=(TooltipBinding){widget,text};
  const lv_event_code_t events[]={LV_EVENT_HOVER_OVER,LV_EVENT_HOVER_LEAVE,
                                LV_EVENT_PRESSED,LV_EVENT_LONG_PRESSED,LV_EVENT_DELETE};
  for(unsigned i=0;i<sizeof(events)/sizeof(events[0]);i++)
    lv_obj_add_event_cb(widget,tooltip_event,events[i],(void *)text);
}

static void tooltip_focus_current(void)
{
  lv_obj_t *widget=NULL;
  if(!g_keyboard_open){
    if(g_rom_picker_active){
      int row=g_rom_picker_selected-g_rom_picker_first;
      if(row>=0 && row<ROM_PICKER_VISIBLE_ROWS)widget=g_rom_picker_rows[row];
    }else if(g_toolbar_focus>=0 && g_toolbar_focus<TOOLBAR_BUTTON_COUNT)widget=g_toolbar_buttons[g_toolbar_focus];
    else if(g_settings_mode && g_settings_focus>=0 && g_settings_focus<SETTINGS_FOCUS_COUNT)widget=g_settings_focus_widgets[g_settings_focus];
    else if(g_detail_index>=0 && g_detail_focus>=0 && g_detail_focus<DETAIL_ACTION_COUNT)widget=g_detail_buttons[g_detail_focus];
  }
  if(widget!=g_tooltip_focus){
    g_tooltip_focus=widget;tooltip_queue(widget,tooltip_text_for(widget),false);
  }
}

static void tooltip_poll(void)
{
  if(!g_tooltips_enabled){tooltip_hide();return;}
  if(!g_tooltip_target)return;
  if(!lv_obj_is_visible(g_tooltip_target)){tooltip_hide();return;}
  uint32_t age=lv_tick_get()-g_tooltip_since;
  if(age>=6650U){tooltip_hide();return;}
  if(age<650U || g_tooltip_visible)return;
  if(!g_tooltip_label){
    g_tooltip_label=lv_label_create(lv_display_get_layer_top(g_display));
    lv_obj_remove_style_all(g_tooltip_label);
    lv_obj_remove_flag(g_tooltip_label,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_font(g_tooltip_label,ui_font(),0);
    lv_obj_set_style_text_color(g_tooltip_label,lv_color_hex(COL_TEXT),0);
    lv_obj_set_style_bg_color(g_tooltip_label,lv_color_hex(COL_PANEL),0);
    lv_obj_set_style_bg_opa(g_tooltip_label,LV_OPA_COVER,0);
    lv_obj_set_style_border_color(g_tooltip_label,lv_color_hex(COL_ACCENT),0);
    lv_obj_set_style_border_width(g_tooltip_label,1,0);
    lv_obj_set_style_radius(g_tooltip_label,ui_px(3),0);
    lv_obj_set_style_pad_all(g_tooltip_label,ui_px(5),0);
    lv_label_set_long_mode(g_tooltip_label,LV_LABEL_LONG_WRAP);
  }
  lv_label_set_text(g_tooltip_label,g_tooltip_text);
  int width=ui_px(245);if(width>g_canvas_w-ui_px(8))width=g_canvas_w-ui_px(8);
  lv_obj_set_width(g_tooltip_label,width);lv_obj_update_layout(g_tooltip_label);
  lv_area_t anchor;lv_obj_get_coords(g_tooltip_target,&anchor);
  int height=lv_obj_get_height(g_tooltip_label),pad=ui_px(4),x=anchor.x1,y=anchor.y2+pad;
  if(x+width>g_canvas_w-pad)x=g_canvas_w-pad-width;if(x<pad)x=pad;
  if(y+height>g_canvas_h-pad)y=anchor.y1-height-pad;if(y<pad)y=pad;
  lv_obj_set_pos(g_tooltip_label,x,y);lv_obj_remove_flag(g_tooltip_label,LV_OBJ_FLAG_HIDDEN);
  g_tooltip_visible=true;
}

static void tooltip_configure(void)
{
  static const char *toolbar[TOOLBAR_BUTTON_COUNT]={
    "Storage: browse apps on the SD card and USB storage.",
    "LAN: find your local PAX Supply server and browse its apps.",
    "Store: browse and install apps from the GitHub PAX Store.",
    "Archive: search Internet Archive collections and download files.",
    "Favorites: show your starred apps.","Settings: storage, network, display and launcher preferences.",
    "Add or remove the selected app from Favorites.","Search or filter this list. Select the keyboard to type on screen.",
    "Clear the search text.","Show or hide the on-screen keyboard.",
    "Refresh the current source and its app list.","Go back to the parent Archive collection or item."
  };
  static const char *settings[SETTINGS_FOCUS_COUNT]={
    "Adjust the master playback and interface volume.","Adjust screen brightness manually.",
    "Use the light sensor to adjust brightness automatically, when supported.","Choose an SD background image, or select None.",
    "Choose the Internet Archive collection to search.","Enter your Archive.org account email.",
    "Enter your Archive.org password. It stays masked in the interface.","Sign in to Archive.org, or sign out of the saved session.",
    "Enter your Archive S3 access key.","Enter your Archive S3 secret key.",
    "Save your S3 credentials on the SD card.","Check whether Archive.org accepts your saved S3 keys.",
    "DHCP obtains network addresses automatically. Turn it off to enter static IPv4 settings.",
    "Stage a static IPv4 address. Apply Network saves the change.","Stage the IPv4 subnet mask.",
    "Stage the router or gateway address.","Stage the DNS server used to resolve internet names.",
    "Apply the staged DHCP or static IPv4 network configuration.",
    "Mount the SD card, or eject it safely after active transfers finish.",
    "Mount USB storage, or eject it safely after active transfers finish.",
    "Show app details before launching a selected app.","Close Settings and return to the app list.",
    "Hide the .pax filename extension in the app list.","Show file sizes and dates in the app list.",
    "Choose a theme. Left and Right change the selection.",
    "Use auto to find PAX Supply, or enter your own HTTP(S) server address.",
    "Enter a Wi-Fi name manually, or choose Scan Nearby.",
    "Enter the selected network's password; leave blank for an open network.",
    "Connect using the staged Wi-Fi name and password.","Choose how the app list is sorted.",
    "Choose automatic, byte, KB or MB file-size display units.",
    "Choose the approved C6 firmware, an SD file, or a web URL. Board compatibility is verified.",
    "Verify the C6 firmware file without installing it.","Install the verified C6 firmware. Keep power connected during the update.",
    "Check GitHub for firmware built for this P4 board.","Install the checked P4 firmware and restart after confirmation.",
    "Scan nearby Wi-Fi networks. Selecting one stages its name until Apply Wi-Fi.",
    "Turn contextual help on or off. This preference is saved on the SD card."
  };
  for(int i=0;i<TOOLBAR_BUTTON_COUNT;i++)tooltip_bind(g_toolbar_buttons[i],toolbar[i]);
  for(int i=0;i<SETTINGS_FOCUS_COUNT;i++)tooltip_bind(g_settings_focus_widgets[i],settings[i]);
  if(!g_settings_mode && g_detail_index>=0)for(int i=0;i<g_detail_action_count;i++)tooltip_bind(g_detail_buttons[i],
      i==0 ? "Launch this app. Choose a game file if one is required." :
      i==1 ? "Download and install this app to the SD card." :
      i==2 ? "Remove the installed app from the SD card." :
      (i==3 && g_detail_extract_recipe.valid) ? "Open Recipe to prepare this app's game data." :
      "Return to the app list.");
  for(int i=0;i<VISIBLE_ROWS;i++)tooltip_bind(g_buttons[i],"Select this app to view details or launch it, according to your Settings preference.");
  if(g_rom_picker_active)for(int i=0;i<ROM_PICKER_VISIBLE_ROWS;i++)tooltip_bind(g_rom_picker_rows[i],"Select the game file to use when launching this app.");
  tooltip_bind(g_status_clock,"Clock: select to switch between time and date.");
  tooltip_bind(g_status_clock_icon,"Clock: select to switch between time and date.");
  tooltip_bind(g_status_network_icon,"Current network connection. Open Settings for addresses and connection details.");
  tooltip_bind(g_status_battery_icon,"Device battery status, when supported by this board.");
  tooltip_bind(g_status_battery_level,"Device battery charge, when supported by this board.");
  for(int i=0;i<2;i++)tooltip_bind(g_archive_s3_eye_buttons[i],"Show or hide this S3 credential.");
  lv_obj_add_event_cb(lv_display_get_screen_active(g_display),tooltip_event,LV_EVENT_PRESSED,NULL);
  lv_obj_t *scroll_views[]={g_settings_panel,g_list,g_detail_info_view};
  for(unsigned i=0;i<3;i++)if(scroll_views[i])lv_obj_add_event_cb(scroll_views[i],tooltip_event,LV_EVENT_SCROLL_BEGIN,NULL);
  g_tooltip_focus=NULL;tooltip_focus_current();
}
