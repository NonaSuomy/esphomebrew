/* On-panel media browser; relay/network work continues while requests wait. */
typedef struct {
  char id[MEDIA_ID];
  unsigned offset, selected;
} browser_parent_t;
typedef struct {
  media_listing_t listing;
  browser_parent_t parents[16];
  char id[MEDIA_ID], note[MEDIA_ERROR];
  unsigned depth, selected, top;
  int sd, paused, muted;
} browser_t;
static browser_t *browser;
static int browser_ensure(void) {
  if (browser)
    return 1;
  browser = svc->mem_alloc(sizeof *browser);
  if (!browser) {
    status = "Not enough memory for the media browser";
    browser_active = 0;
    return 0;
  }
  zero(browser, sizeof *browser);
  copy(browser->note, "Choose a folder or media file", sizeof browser->note);
  return 1;
}
static int browser_rows(void) {
  int n = (sh - 160) / (sw >= 640 ? 40 : 28);
  return n < 1 ? 1 : n;
}
static void browser_number(char *out, size_t cap, unsigned value) {
  char digits[12];
  unsigned n = 0;
  do {
    digits[n++] = (char)('0' + value % 10);
    value /= 10;
  } while (value);
  size_t used = length(out);
  while (n && used + 1 < cap)
    out[used++] = digits[--n];
  out[used] = 0;
}
static void browser_append(char *out, const char *value, size_t cap) {
  size_t n = length(out);
  if (n < cap)
    copy(out + n, value, cap - n);
}
static void browser_label(char *out, const char *value, size_t cap) {
  size_t n = 0;
  for (const unsigned char *p = (const unsigned char *)value; *p && n + 1 < cap;
       p++) {
    if ((*p & 0xc0) == 0x80)
      continue;
    out[n++] = *p >= 128 ? '?' : *p < 32 ? ' ' : (char)*p;
  }
  out[n] = 0;
}
static void browser_draw(void) {
  if (!browser_ensure())
    return;
  fill(0, 0, sw, sh, 0x1084);
  text(12, 14, browser->sd ? "MEDIA / SD CARD" : "MEDIA / HOME ASSISTANT",
       sw >= 640 ? 2 : 1, 0x8e7f);
  button(sw - 90, 6, 78, 28, "CLOSE", 0);
  const char *nav[] = {"HA", "SD", "UP", "LOAD", "PREV", "NEXT"};
  int width = (sw - 24) / 6;
  for (int i = 0; i < 6; i++)
    button(12 + i * width, 42, width - 4, 30, nav[i],
           i == (browser->sd ? 1 : 0));
  char caption[256];
  browser_label(caption,
                browser->listing.title[0]
                    ? browser->listing.title
                    : (browser->sd ? "/sd/media" : "Media sources"),
                sizeof caption);
  browser_append(caption, "  [", sizeof caption);
  browser_number(caption, sizeof caption,
                 browser->listing.total ? browser->listing.offset + 1 : 0);
  browser_append(caption, "-", sizeof caption);
  browser_number(caption, sizeof caption,
                 browser->listing.offset + browser->listing.count);
  browser_append(caption, " / ", sizeof caption);
  browser_number(caption, sizeof caption, browser->listing.total);
  browser_append(caption, "]", sizeof caption);
  text(12, 82, caption, 1, 0xc618);
  const unsigned rows = (unsigned)browser_rows();
  int height = sw >= 640 ? 40 : 28;
  if (browser->selected < browser->top)
    browser->top = browser->selected;
  if (browser->selected >= browser->top + rows)
    browser->top = browser->selected - rows + 1;
  for (unsigned row = 0;
       row < rows && row + browser->top < browser->listing.count; row++) {
    unsigned index = row + browser->top;
    const media_item_t *item = &browser->listing.items[index];
    char line[180], title[MEDIA_TITLE];
    browser_label(title, item->title, sizeof title);
    copy(line,
         item->directory         ? "[DIR] "
         : media_kind(item) == 1 ? "[MUSIC] "
         : media_kind(item) == 2 ? "[VIDEO] "
                                 : "[--] ",
         sizeof line);
    browser_append(line, title, sizeof line);
    button(12, 96 + (int)row * height, sw - 24, height - 3, line,
           index == browser->selected);
  }
  if (!browser->listing.count)
    text(18, 112,
         browser_busy ? "Loading..."
                      : "No media here. Choose HA, SD, or another folder.",
         1, 0xc618);
  const char *controls[] = {browser->paused ? "RESUME" : "PAUSE", "STOP",
                            browser->muted ? "UNMUTE" : "MUTE", "VIEW"};
  width = (sw - 24) / 4;
  for (int i = 0; i < 4; i++)
    button(12 + i * width, sh - 54, width - 4, 29, controls[i], 0);
  char note[MEDIA_ERROR];
  browser_label(note,
                browser_busy ? "Loading... Esc cancels the request."
                             : browser->note,
                sizeof note);
  text(12, sh - 16, note, 1, 0xffff);
  svc->display_flush();
  dirty = 0;
}
static int browser_pump(void) { return relay_pump() || browser_cancel; }
static int browser_load(int sd, const char *id, unsigned offset) {
  media_listing_t *listing = svc->mem_alloc(sizeof *listing);
  if (!listing) {
    copy(browser->note, "Not enough memory for media listing",
         sizeof browser->note);
    return -1;
  }
  browser_busy = 1;
  browser_cancel = 0;
  dirty = 1;
  browser_draw();
  char error[MEDIA_ERROR];
  int result = sd ? media_sd_list(svc, id, offset, listing, error, browser_pump)
                  : media_browse(svc, id, offset, listing, error, browser_pump);
  browser_busy = 0;
  if (!result) {
    browser->listing = *listing;
    copy(browser->id, id, sizeof browser->id);
    browser->sd = sd;
    browser->selected = browser->top = 0;
    copy(browser->note, "Tap a folder or file. Arrows + Enter also work.",
         sizeof browser->note);
  } else
    copy(browser->note, error, sizeof browser->note);
  svc->mem_free(listing);
  dirty = 1;
  return result;
}
static void browser_move(int delta) {
  if (!browser || !browser->listing.count)
    return;
  if (delta < 0) {
    if (browser->selected)
      browser->selected--;
  } else if (browser->selected + 1 < browser->listing.count)
    browser->selected++;
  dirty = 1;
}
static void browser_hit(int x, int y) {
  if (browser_busy)
    return;
  if (y >= 6 && y < 34 && x >= sw - 90) {
    browser_active = 0;
    dirty = 1;
    return;
  }
  if (y >= 42 && y < 72 && x >= 12 && x < sw - 12) {
    unsigned col = (unsigned)(x - 12) / (unsigned)((sw - 24) / 6);
    const int actions[] = {2, 3, 4, 1, 5, 6};
    if (col < 6)
      browser_action = actions[col];
    return;
  }
  if (y >= sh - 54 && y < sh - 25 && x >= 12 && x < sw - 12) {
    unsigned col = (unsigned)(x - 12) / (unsigned)((sw - 24) / 4);
    if (col < 4)
      browser_action = col == 3 ? 11 : 8 + (int)col;
    return;
  }
  int height = sw >= 640 ? 40 : 28;
  if (y >= 96 && y < sh - 64) {
    unsigned at = browser->top + (unsigned)(y - 96) / (unsigned)height;
    if (at < browser->listing.count) {
      browser->selected = at;
      browser_action = 7;
      dirty = 1;
    }
  }
}
static void browser_inputs(unsigned pressed, int x, int y, int touched) {
  if (!browser_ensure())
    return;
  if (!browser_busy) {
    if (pressed & (1u << PAX_INPUT_DOWN))
      browser_move(1);
    if (pressed & (1u << PAX_INPUT_UP))
      browser_move(-1);
    if (pressed & (1u << PAX_INPUT_LEFT))
      browser_action = 5;
    if (pressed & (1u << PAX_INPUT_RIGHT))
      browser_action = 6;
    if (pressed & (1u << PAX_INPUT_A))
      browser_action = 7;
    if (pressed & (1u << PAX_INPUT_B))
      browser_action = browser->depth ? 4 : 11;
    if (pressed & (1u << PAX_INPUT_X))
      browser_action = browser->sd ? 2 : 3;
    if (pressed & (1u << PAX_INPUT_Y))
      browser_action = 1;
    if (pressed & (1u << PAX_INPUT_START))
      browser_action = 8;
    if (pressed & (1u << PAX_INPUT_SELECT))
      browser_action = 9;
    if (pressed & (1u << PAX_INPUT_L))
      browser_action = 10;
    if (touched && !touch_before)
      browser_hit(x, y);
  }
  if (svc->input_mouse_read) {
    int dx = 0, dy = 0, mb = 0;
    if (svc->input_mouse_read(&dx, &dy, &mb)) {
      mouse_x += dx;
      mouse_y += dy;
      if (mouse_x < 0)
        mouse_x = 0;
      if (mouse_x >= sw)
        mouse_x = sw - 1;
      if (mouse_y < 0)
        mouse_y = 0;
      if (mouse_y >= sh)
        mouse_y = sh - 1;
      if ((mb & 1) && !(mouse_buttons & 1))
        browser_hit(mouse_x, mouse_y);
      mouse_buttons = mb;
    }
  }
  if (svc->input_mouse_scroll_read) {
    int wheel = 0;
    if (svc->input_mouse_scroll_read(&wheel) && !browser_busy && wheel)
      browser_move(wheel > 0 ? -1 : 1);
  }
  if (svc->input_keyboard_read) {
    pax_keyboard_event_t e;
    int budget = 32;
    while (budget-- && svc->input_keyboard_read(&e))
      if (e.down) {
        if (browser_busy) {
          if (e.key == 27)
            browser_cancel = 1;
          continue;
        }
        if (e.key == PAX_KEY_DOWN)
          browser_move(1);
        else if (e.key == PAX_KEY_UP)
          browser_move(-1);
        else if (e.key == PAX_KEY_LEFT)
          browser_action = 5;
        else if (e.key == PAX_KEY_RIGHT)
          browser_action = 6;
        else if (e.key == 13)
          browser_action = 7;
        else if (e.key == 8 || e.key == 127)
          browser_action = 4;
        else if (e.key == 27)
          browser_action = 11;
        else if (e.key == ' ')
          browser_action = 8;
        else if (e.key == 'm' || e.key == 'M')
          browser_action = 10;
        else if (e.key == 'x' || e.key == 'X')
          browser_action = 9;
        else if (e.key == 'r' || e.key == 'R')
          browser_action = 1;
      }
  }
}
static int browser_video_canvas(void) {
  media_mode = 1;
  if (sw < 800)
    return 0;
  if (relay_peer >= 0) {
    svc->net_udp_close(relay_peer);
    relay_peer = -1;
    relay_close(&relay);
  }
  audio_buffer_finish(&audio_buffer, 0);
  svc->mem_free(relay_pixels);
  relay_pixels = 0;
  media_canvas(1);
  relay_pixels = svc->mem_alloc((size_t)sw * sh * 2);
  if (!relay_pixels) {
    back = 1;
    status = "Not enough memory for the video canvas";
    return -1;
  }
  zero(relay_pixels, (size_t)sw * sh * 2);
  return 0;
}
static void browser_run(void) {
  if (!browser_active || browser_busy || !browser_action || !browser_ensure())
    return;
  const int action = browser_action;
  browser_action = 0;
  if (action == 11) {
    browser_active = 0;
    dirty = 1;
    return;
  }
  if (action == 1)
    browser_load(browser->sd, browser->id, browser->listing.offset);
  else if (action == 2 || action == 3) {
    if (!browser_load(action == 3, action == 3 ? "/sd/media" : "", 0))
      browser->depth = 0;
  } else if (action == 4) {
    if (browser->depth) {
      browser_parent_t *parent = &browser->parents[browser->depth - 1];
      if (!browser_load(browser->sd, parent->id, parent->offset)) {
        browser->depth--;
        browser->selected = parent->selected;
      }
    } else
      copy(browser->note, "At source root. Choose HA or SD, or CLOSE.",
           sizeof browser->note);
  } else if (action == 5 || action == 6) {
    unsigned offset = browser->listing.offset;
    if (action == 5 && offset >= MEDIA_PAGE)
      browser_load(browser->sd, browser->id, offset - MEDIA_PAGE);
    if (action == 6 && offset + MEDIA_PAGE < browser->listing.total)
      browser_load(browser->sd, browser->id, offset + MEDIA_PAGE);
  } else if (action == 7 && browser->selected < browser->listing.count) {
    const media_item_t *item = &browser->listing.items[browser->selected];
    if (item->directory) {
      if (browser->depth == 16)
        copy(browser->note, "Folder depth limit reached", sizeof browser->note);
      else {
        browser_parent_t *parent = &browser->parents[browser->depth];
        copy(parent->id, browser->id, sizeof parent->id);
        parent->offset = browser->listing.offset;
        parent->selected = browser->selected;
        char id[MEDIA_ID];
        copy(id, item->id, sizeof id);
        if (!browser_load(browser->sd, id, 0))
          browser->depth++;
      }
    } else if (item->playable && item->id[0] && media_kind(item)) {
      const int kind = media_kind(item);
      if (kind == 2 && browser_video_canvas())
        return;
      char error[MEDIA_ERROR];
      browser_busy = 1;
      browser_cancel = 0;
      dirty = 1;
      if (kind == 2)
        browser_active = 0;
      else
        browser_draw();
      int result = media_play(svc, item, browser->sd, error, browser_pump);
      browser_busy = 0;
      if (result) {
        browser_active = 1;
        copy(browser->note, error, sizeof browser->note);
      } else {
        copy(browser->note, "Playing: ", sizeof browser->note);
        browser_append(browser->note, item->title, sizeof browser->note);
        browser->paused = 0;
      }
    } else
      copy(browser->note, "This item is not an audio or video file",
           sizeof browser->note);
  } else if (action >= 8 && action <= 10) {
    if (relay_peer >= 0 && relay.wire_mode == 2) {
      if (action == 8) {
        relay_media_pause(&relay, !relay.avi_paused);
        audio_buffer_pause(&audio_buffer, relay.avi_paused);
        browser->paused = relay.avi_paused;
      }
      if (action == 9) {
        relay_media_stop(&relay);
        browser->paused = 0;
      }
      if (action == 10) {
        relay_muted = !relay_muted;
        audio_buffer_mute(&audio_buffer, relay_muted);
        browser->muted = relay_muted;
      }
      copy(browser->note, "Video control sent", sizeof browser->note);
    } else {
      const char *command = action == 8 ? (browser->paused ? "resume" : "pause")
                            : action == 9    ? "stop"
                            : browser->muted ? "unmute"
                                             : "mute";
      char error[MEDIA_ERROR];
      browser_busy = 1;
      browser_cancel = 0;
      dirty = 1;
      browser_draw();
      int result = media_control(svc, command, error, browser_pump);
      browser_busy = 0;
      if (result)
        copy(browser->note, error, sizeof browser->note);
      else {
        if (action == 8)
          browser->paused = !browser->paused;
        if (action == 9)
          browser->paused = 0;
        if (action == 10)
          browser->muted = !browser->muted;
        copy(browser->note, "Audio control sent", sizeof browser->note);
      }
    }
  }
  dirty = 1;
}
