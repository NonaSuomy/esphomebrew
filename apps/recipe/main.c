/*
 * recipe — a standalone PAX for recipe-driven game-data extraction.
 *
 * This app owns the ZIP/ISO/CUE parsing and extraction buffers. launcher_ui
 * only hands it a local app-info JSON path, so the disc parser is unloaded
 * with this PAX instead of remaining in the launcher or emulator.
 */
#define PAX_APP_SIDE 1
#include "psram_app.h"
#include "lvgl.h"
#include "recipe_sha256.h"
#include "../launcher_ui/iso9660_extract.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SCREEN_W 512
#define SCREEN_H 300
#define TOUCH_FRAME_W 800
#define TOUCH_FRAME_H 480
#define MAX_JSON_BYTES (192U * 1024U)
#define MAX_MEDIA 16
#define MAX_FILES 512
#define PATH_CAP 256
#define ENTRY_CAP 192
#define NAME_CAP 96
#define STATUS_CAP 192
#define MAX_FOLDERS 32
#define ZIP_LIST_CAP 8192
#define MAX_INCOMING_FILES 32
#define MAX_IMAGE_BYTES (1024U * 1024U * 1024U)
#define MAX_FILE_BYTES (512U * 1024U * 1024U)

#define COL_BG      0x071525
#define COL_PANEL   0x102844
#define COL_ACCENT  0x25C7FF
#define COL_TEXT    0xF2F7FF
#define COL_MUTED   0x8BA7C4
#define COL_OK      0x4ADE80
#define COL_WARN    0xFBBF24
#define COL_ERROR   0xF87171

typedef struct {
  char source[ENTRY_CAP];
  char target[PATH_CAP];
  char url[512];
  unsigned expected_size;
  char sha256[65];
  bool optional;
  bool present;
} recipe_file_t;

typedef struct {
  char format[24];
  char entry[ENTRY_CAP];
  char zip_name[NAME_CAP];
  char iso_name[NAME_CAP];
  char download_label[NAME_CAP];
  char archive_url[512];
  unsigned archive_size;
  char archive_sha256[65];
  bool auto_entry;
  int first_file;
  int file_count;
} recipe_media_t;

typedef struct {
  char archive_dir[PATH_CAP];
  recipe_media_t media[MAX_MEDIA];
  recipe_file_t files[MAX_FILES];
  int media_count;
  int file_count;
  unsigned version;
  char instructions[4096];
  char folders[MAX_FOLDERS][PATH_CAP];
  int folder_count;
} recipe_t;

typedef int (*zip_extract_named_fn)(const char *, const char *, const char *,
                                    uint32_t, uint32_t *);

static const app_services_t *g_svc;
static recipe_t g_recipe;
static uint16_t *g_canvas;
static int g_width = SCREEN_W;
static int g_height = SCREEN_H;
static lv_display_t *g_display;
static lv_indev_t *g_touch_indev;
static bool g_touch_was_down;
static lv_obj_t *g_status_label;
static lv_obj_t *g_progress_bar;
static lv_obj_t *g_progress_label;
static char g_status[STATUS_CAP] = "Preparing installer...";
static uint32_t g_done;
static uint32_t g_total;
static uint64_t g_byte_done;
static uint64_t g_byte_total;
static uint64_t g_last_progress_ui_us;
static uint64_t g_last_progress_log_us;
static uint64_t g_progress_sample_us;
static uint64_t g_progress_sample_bytes;
static uint64_t g_byte_rate_bps;
static bool g_byte_progress_active;
static bool g_success;
static char g_incoming_names[MAX_INCOMING_FILES][ENTRY_CAP];
static int g_incoming_count;
static int g_download_rows;
static int g_download_media[MAX_MEDIA + 1];
static int g_focused_incoming;
static volatile int g_requested_incoming = -1;
static volatile bool g_exit_requested;
static lv_obj_t *g_incoming_list;
static lv_obj_t *g_incoming_buttons[MAX_INCOMING_FILES];
static lv_obj_t *g_empty_label;
static lv_obj_t *g_instructions_overlay;
static lv_obj_t *g_back_button;
static bool g_processing_media;

static size_t text_len(const char *s)
{
  size_t n = 0;
  if (s != NULL) while (s[n] != '\0') n++;
  return n;
}

static bool text_copy(char *dst, size_t cap, const char *src)
{
  if (dst == NULL || cap == 0 || src == NULL) return false;
  size_t n = text_len(src);
  if (n >= cap) n = cap - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
  return n == text_len(src);
}

static bool text_append(char *dst, size_t cap, const char *src)
{
  const size_t used = text_len(dst), add = text_len(src);
  if (dst == NULL || src == NULL || used + add >= cap) return false;
  memcpy(dst + used, src, add + 1);
  return true;
}

static bool append_u32(char *dst, size_t cap, uint32_t value)
{
  char reverse[10];
  size_t n = 0;
  do {
    reverse[n++] = (char)('0' + (value % 10U));
    value /= 10U;
  } while (value != 0 && n < sizeof(reverse));
  while (n != 0) {
    char digit[2] = {reverse[--n], '\0'};
    if (!text_append(dst, cap, digit)) return false;
  }
  return true;
}

static void append_mib_tenths(char *dst, size_t cap, uint64_t bytes)
{
  const uint64_t tenths = (bytes * 10U) / (1024U * 1024U);
  (void)append_u32(dst, cap, (uint32_t)(tenths / 10U));
  (void)text_append(dst, cap, ".");
  char digit[2] = {(char)('0' + (tenths % 10U)), '\0'};
  (void)text_append(dst, cap, digit);
}

static int skip_ws(const char *s, int len, int i)
{
  while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
  return i;
}

static int json_string_end(const char *s, int len, int start)
{
  if (start < 0 || start >= len || s[start] != '"') return -1;
  bool escaped = false;
  for (int i = start + 1; i < len; ++i) {
    if (escaped) escaped = false;
    else if (s[i] == '\\') escaped = true;
    else if (s[i] == '"') return i + 1;
  }
  return -1;
}

static int json_value_end(const char *s, int len, int start)
{
  start = skip_ws(s, len, start);
  if (start >= len) return -1;
  if (s[start] == '"') return json_string_end(s, len, start);
  if (s[start] == '{' || s[start] == '[') {
    int braces = 0, brackets = 0;
    bool in_string = false, escaped = false;
    for (int i = start; i < len; ++i) {
      const char c = s[i];
      if (in_string) {
        if (escaped) escaped = false;
        else if (c == '\\') escaped = true;
        else if (c == '"') in_string = false;
        continue;
      }
      if (c == '"') in_string = true;
      else if (c == '{') braces++;
      else if (c == '}') { if (--braces == 0 && brackets == 0) return i + 1; }
      else if (c == '[') brackets++;
      else if (c == ']') { if (--brackets == 0 && braces == 0) return i + 1; }
    }
    return -1;
  }
  int i = start;
  while (i < len && s[i] != ',' && s[i] != ']' && s[i] != '}' &&
         s[i] != ' ' && s[i] != '\t' && s[i] != '\r' && s[i] != '\n') i++;
  return i > start ? i : -1;
}

static bool json_string_decode(const char *s, int len, int start,
                               char *out, size_t cap)
{
  if (out == NULL || cap == 0 || start < 0 || start >= len || s[start] != '"') return false;
  size_t used = 0;
  for (int i = start + 1; i < len; ++i) {
    char c = s[i];
    if (c == '"') { out[used] = '\0'; return true; }
    if (c == '\\') {
      if (++i >= len) return false;
      c = s[i];
      if (c == 'n') c = '\n';
      else if (c == 'r') c = '\r';
      else if (c == 't') c = '\t';
      else if (c == 'u') {
        unsigned code=0;
        for(int n=0;n<4;n++){
          if(++i>=len)return false;char h=s[i];
          unsigned v=h>='0'&&h<='9'?(unsigned)(h-'0'):h>='a'&&h<='f'?(unsigned)(h-'a'+10):h>='A'&&h<='F'?(unsigned)(h-'A'+10):16;
          if(v>15)return false;code=(code<<4)|v;
        }
        if(!code || (code>=0xd800 && code<=0xdfff))return false;
        unsigned count=code<0x80?1:code<0x800?2:3;if(used+count>=cap)return false;
        if(count==1)out[used++]=(char)code;
        else if(count==2){out[used++]=(char)(0xc0|(code>>6));out[used++]=(char)(0x80|(code&63));}
        else {out[used++]=(char)(0xe0|(code>>12));out[used++]=(char)(0x80|((code>>6)&63));out[used++]=(char)(0x80|(code&63));}
        continue;
      }
      else if (c != '"' && c != '\\' && c != '/') return false;
    }
    if (used + 1 >= cap) return false;
    out[used++] = c;
  }
  return false;
}

static bool json_object_field(const char *s, int len, int object_start,
                              const char *key, int *value_start, int *value_end)
{
  if (object_start < 0 || object_start >= len || s[object_start] != '{') return false;
  int i = object_start + 1;
  char decoded[96];
  while ((i = skip_ws(s, len, i)) < len && s[i] != '}') {
    const int key_start = i;
    const int key_end = json_string_end(s, len, key_start);
    if (key_end < 0 || !json_string_decode(s, key_end, key_start, decoded, sizeof(decoded))) return false;
    i = skip_ws(s, len, key_end);
    if (i >= len || s[i++] != ':') return false;
    i = skip_ws(s, len, i);
    const int end = json_value_end(s, len, i);
    if (end < 0) return false;
    if (strcmp(decoded, key) == 0) {
      *value_start = i;
      *value_end = end;
      return true;
    }
    i = skip_ws(s, len, end);
    if (i < len && s[i] == ',') i++;
    else if (i < len && s[i] != '}') return false;
  }
  return false;
}

static bool json_get_string(const char *s, int len, int object_start,
                            const char *key, char *out, size_t cap)
{
  int start, end;
  return json_object_field(s, len, object_start, key, &start, &end) &&
         end > start && s[start] == '"' && json_string_decode(s, end, start, out, cap);
}

static bool json_get_bool(const char *s, int len, int object_start, const char *key)
{
  int start, end;
  return json_object_field(s, len, object_start, key, &start, &end) &&
         end - start == 4 && memcmp(s + start, "true", 4) == 0;
}

static bool json_get_uint(const char *s, int len, int object_start,
                          const char *key, unsigned *out)
{
  int start, end;
  if (!json_object_field(s, len, object_start, key, &start, &end) || start >= end ||
      s[start] < '0' || s[start] > '9') return false;
  unsigned value = 0;
  while (start < end && s[start] >= '0' && s[start] <= '9') {
    const unsigned digit = (unsigned)(s[start++] - '0');
    if (value > (UINT32_MAX - digit) / 10U) return false;
    value = value * 10U + digit;
  }
  if (start != end) return false;
  *out = value;
  return true;
}

static bool path_under_roms(const char *path)
{
  if (path == NULL || !(strncmp(path, "/sd/", 4) == 0 || strncmp(path, "/usb0/", 6) == 0)) return false;
  if (path[text_len(path)-1] == '/') return false;
  for (const char *p=path; *p; ++p)
    if (*p=='\\' || (*p=='/' && p[1]=='/') ||
        (*p=='.' && (p==path || p[-1]=='/') &&
         (p[1]=='/' || p[1]=='\0' || (p[1]=='.' && (p[2]=='/' || p[2]=='\0'))))) return false;
  return true;
}

static bool download_url_valid(const char *url, unsigned version)
{
  if (version == 1) return strncmp(url, "https://archive.org/download/", 29) == 0;
  if (strncmp(url, "https://", 8) != 0 || !url[8]) return false;
  const char *slash = strchr(url + 8, '/');
  if (!slash || slash == url + 8) return false;
  for (const char *p=url+8; p<slash; ++p) if (*p=='@' || *p==' ' || *p=='\\') return false;
  return true;
}

static bool relative_path_valid(const char *path)
{
  if (path == NULL || path[0] == '\0' || path[0] == '/' || path[0] == '\\') return false;
  size_t segment = 0;
  bool dot = true, dotdot = true;
  for (const char *p = path; ; ++p) {
    const char c = *p;
    if (c == '\\') return false;
    if (c == '/' || c == '\0') {
      if (segment == 0 || dot || dotdot) return false;
      if (c == '\0') return true;
      segment = 0; dot = true; dotdot = true;
      continue;
    }
    if (segment >= 127) return false;
    if (segment == 0) dot = dotdot = c == '.';
    else if (segment == 1) { dot = false; dotdot = dotdot && c == '.'; }
    else { dot = false; dotdot = false; }
    segment++;
  }
}

static bool ends_with_ci(const char *s, const char *suffix)
{
  const size_t n = text_len(s), m = text_len(suffix);
  if (n < m) return false;
  for (size_t i = 0; i < m; ++i) {
    char a = s[n - m + i], b = suffix[i];
    if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
    if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
    if (a != b) return false;
  }
  return true;
}

static bool array_next(const char *s, int len, int *cursor, int *start, int *end)
{
  int i = skip_ws(s, len, *cursor);
  if (i < len && s[i] == ',') i = skip_ws(s, len, i + 1);
  if (i >= len || s[i] == ']') { *cursor = i; return false; }
  const int value_end = json_value_end(s, len, i);
  if (value_end < 0) return false;
  *start = i; *end = value_end; *cursor = value_end;
  return true;
}

static bool parse_recipe(const char *json, int length)
{
  memset(&g_recipe, 0, sizeof(g_recipe));
  int extract_start, extract_end;
  if (!json_object_field(json, length, 0, "extract", &extract_start, &extract_end) ||
      json[extract_start] != '{') return false;
  unsigned version = 0;
  if (!json_get_uint(json, length, extract_start, "version", &version) || (version != 1 && version != 2) ||
      !json_get_string(json, length, extract_start, "archive_dir",
                       g_recipe.archive_dir, sizeof(g_recipe.archive_dir)) ||
      !path_under_roms(g_recipe.archive_dir)) return false;

  g_recipe.version = version;
  if (version == 2) {
    if (!json_get_string(json, length, extract_start, "instructions", g_recipe.instructions, sizeof(g_recipe.instructions))) return false;
    int fs, fe;
    if (json_object_field(json, length, extract_start, "folders", &fs, &fe)) {
      if (json[fs] != '[') return false;
      int fc=fs+1, vs, ve;
      while (array_next(json, fe, &fc, &vs, &ve)) {
        if (g_recipe.folder_count >= MAX_FOLDERS || json[vs] != '"' ||
            !json_string_decode(json, ve, vs, g_recipe.folders[g_recipe.folder_count], PATH_CAP) ||
            !path_under_roms(g_recipe.folders[g_recipe.folder_count])) return false;
        g_recipe.folder_count++;
      }
    }
  }
  int media_start, media_end;
  if (!json_object_field(json, length, extract_start, "media", &media_start, &media_end) ||
      json[media_start] != '[') return false;
  int cursor = media_start + 1;
  while (true) {
    int item_start, item_end;
    if (!array_next(json, media_end, &cursor, &item_start, &item_end)) break;
    if (g_recipe.media_count >= MAX_MEDIA || json[item_start] != '{') return false;
    recipe_media_t *media = &g_recipe.media[g_recipe.media_count];
    if (!json_get_string(json, item_end, item_start, "format", media->format,
                         sizeof(media->format))) return false;
    media->auto_entry = json_get_bool(json, item_end, item_start, "auto_entry");
    const bool has_entry = json_get_string(json, item_end, item_start, "entry",
                                           media->entry, sizeof(media->entry));
    const bool simple = version == 2 && (strcmp(media->format, "download") == 0 || strcmp(media->format, "zip-files") == 0);
    if (!simple && media->auto_entry) {
      if (strcmp(media->format, "zip-iso9660") != 0 || has_entry) return false;
    } else if (!simple && (!has_entry || !relative_path_valid(media->entry))) {
      return false;
    }
    (void)json_get_string(json, item_end, item_start, "zip_name", media->zip_name,
                          sizeof(media->zip_name));
    (void)json_get_string(json, item_end, item_start, "iso_name", media->iso_name, sizeof(media->iso_name));
    if (media->iso_name[0] != '\0' && (!ends_with_ci(media->iso_name, ".iso") ||
        strchr(media->iso_name, '/') != NULL || strchr(media->iso_name, '\\') != NULL)) return false;
    (void)json_get_string(json, item_end, item_start, "download_label", media->download_label, sizeof(media->download_label));
    if (media->zip_name[0] != '\0' &&
        (strchr(media->zip_name, '/') != NULL || strchr(media->zip_name, '\\') != NULL ||
         !ends_with_ci(media->zip_name, ".zip"))) return false;
    if (!simple && !media->auto_entry &&
        (strcmp(media->format, "zip-iso9660") != 0 || !ends_with_ci(media->entry, ".iso")) &&
        (strcmp(media->format, "zip-cue-bin") != 0 || !ends_with_ci(media->entry, ".cue")))
      return false;

    if (version == 2 && strcmp(media->format, "zip-files") == 0) {
      if (!ends_with_ci(media->zip_name, ".zip")) return false;
      int as, ae;
      if (json_object_field(json, item_end, item_start, "archive_url", &as, &ae) &&
          (!json_get_string(json, item_end, item_start, "archive_url", media->archive_url, sizeof(media->archive_url)) ||
           !download_url_valid(media->archive_url, version) ||
           !json_get_uint(json, item_end, item_start, "archive_size", &media->archive_size) ||
           !media->archive_size || media->archive_size > MAX_FILE_BYTES)) return false;
      (void)json_get_string(json, item_end, item_start, "archive_sha256", media->archive_sha256, sizeof(media->archive_sha256));
    }
    int files_start, files_end;
    if (!json_object_field(json, item_end, item_start, "files", &files_start, &files_end) ||
        json[files_start] != '[') return false;
    media->first_file = g_recipe.file_count;
    int file_cursor = files_start + 1;
    while (true) {
      int file_start, file_end;
      if (!array_next(json, files_end, &file_cursor, &file_start, &file_end)) break;
      if (g_recipe.file_count >= MAX_FILES || json[file_start] != '{') return false;
      recipe_file_t *file = &g_recipe.files[g_recipe.file_count];
      if (!json_get_string(json, file_end, file_start, "source", file->source,
                           sizeof(file->source)) ||
          !json_get_string(json, file_end, file_start, "target", file->target,
                           sizeof(file->target)) || !relative_path_valid(file->source) ||
          !path_under_roms(file->target)) return false;
      int url_start, url_end;
      if (json_object_field(json, file_end, file_start, "url", &url_start, &url_end)) {
        if (!json_get_string(json, file_end, file_start, "url", file->url, sizeof(file->url)) ||
            !download_url_valid(file->url, version) ||
            !json_get_uint(json, file_end, file_start, "size", &file->expected_size) ||
            file->expected_size == 0 || file->expected_size > MAX_FILE_BYTES) return false;
      }
      (void)json_get_uint(json, file_end, file_start, "size", &file->expected_size);
      (void)json_get_string(json, file_end, file_start, "sha256", file->sha256, sizeof(file->sha256));
      file->optional = json_get_bool(json, file_end, file_start, "optional");
      for (int i = 0; i < g_recipe.file_count; ++i)
        if (strcmp(g_recipe.files[i].target, file->target) == 0) return false;
      media->file_count++;
      g_recipe.file_count++;
    }
    if (media->file_count == 0) return false;
    g_recipe.media_count++;
  }
  return (g_recipe.media_count > 0 && g_recipe.file_count > 0) || (version == 2 && g_recipe.instructions[0]);
}

/* Older Red Alert app-info JSON on existing cards only listed allied/MAIN.MIX.
 * Add the base-game mix from the same Allied CD recipe at runtime, so updating
 * the installer PAX is enough to repair those cards without overwriting the
 * user's app-info JSON. New JSON already contains this entry and needs no shim.
 */
static bool ensure_redalert_base_mix(const char *json_path)
{
  if (!ends_with_ci(json_path, "/redalert.json")) return true;
  const char *target = "/sd/roms/redalert/REDALERT.MIX";
  for (int i = 0; i < g_recipe.file_count; ++i)
    if (strcmp(g_recipe.files[i].target, target) == 0) return true;

  for (int m = 0; m < g_recipe.media_count; ++m) {
    recipe_media_t *media = &g_recipe.media[m];
    if (strcmp(media->format, "zip-iso9660") != 0 ||
        strcmp(media->zip_name, "cncredalertalliedcd1.zip") != 0 ||
        !media->auto_entry || media->first_file + media->file_count != g_recipe.file_count ||
        g_recipe.file_count >= MAX_FILES) continue;
    recipe_file_t *file = &g_recipe.files[g_recipe.file_count++];
    memset(file, 0, sizeof(*file));
    (void)text_copy(file->source, sizeof(file->source), "INSTALL/REDALERT.MIX");
    (void)text_copy(file->target, sizeof(file->target), target);
    media->file_count++;
    if (g_svc->log_printf != NULL)
      g_svc->log_printf("recipe: added missing legacy Red Alert recipe entry for %s\n", target);
    return true;
  }
  return false;
}

static bool file_exists(const char *path)
{
  pax_file_stat_t info = {0};
  return g_svc->file_stat != NULL && g_svc->file_stat(path, &info) == 0 && !info.is_dir && info.size > 0;
}

/* The cache may have been copied onto the card from a PC, so its filesystem
 * timestamp is not a reliable indication of whether it is usable. Validate
 * the ISO9660 primary volume descriptor directly instead. */
static bool cached_iso_is_valid(const char *path, uint64_t *size_out)
{
  if (size_out != NULL) *size_out = 0;
  if (g_svc == NULL || g_svc->file_stat == NULL || g_svc->file_open == NULL ||
      g_svc->file_read == NULL || g_svc->file_seek == NULL ||
      g_svc->file_close == NULL) return false;

  pax_file_stat_t info = {0};
  if (g_svc->file_stat(path, &info) != 0 || info.is_dir ||
      info.size < (uint64_t)(16U * 2048U + 7U)) return false;

  void *file = g_svc->file_open(path, "rb");
  if (file == NULL) return false;
  uint8_t descriptor[7] = {0};
  const bool read_ok = g_svc->file_seek(file, 16L * 2048L, 0) == 0 &&
                       g_svc->file_read(descriptor, 1, sizeof(descriptor), file) ==
                           sizeof(descriptor);
  (void)g_svc->file_close(file);
  const bool valid = read_ok && descriptor[0] == 1 &&
                     memcmp(&descriptor[1], "CD001", 5) == 0 &&
                     descriptor[6] == 1;
  if (valid && size_out != NULL) *size_out = info.size;
  return valid;
}

static bool make_parent_dirs(const char *path)
{
  char parent[PATH_CAP];
  if (!text_copy(parent, sizeof(parent), path)) return false;
  char *slash = strrchr(parent, '/');
  if (slash == NULL || slash == parent) return false;
  *slash = '\0';
  return g_svc->file_mkdir != NULL && g_svc->file_mkdir(parent) == 0;
}

static bool join_path(char *out, size_t cap, const char *a, const char *b)
{
  return text_copy(out, cap, a) && text_append(out, cap, "/") && text_append(out, cap, b);
}

static bool text_equal_ci(const char *left, const char *right)
{
  if (left == NULL || right == NULL) return false;
  while (*left != '\0' && *right != '\0') {
    char a = *left++, b = *right++;
    if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
    if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
    if (a != b) return false;
  }
  return *left == '\0' && *right == '\0';
}

static int scan_incoming_files(void)
{
  g_incoming_count = 0;
  g_download_rows = 0;
  memset(g_incoming_names, 0, sizeof(g_incoming_names));
  if (g_recipe.version == 2) {
    g_download_media[g_download_rows++] = -1;
    text_copy(g_incoming_names[g_incoming_count++], ENTRY_CAP, "Prepare folders / read instructions");
  }
  for (int m = 0; m < g_recipe.media_count; ++m) {
    bool downloadable = g_recipe.media[m].archive_url[0] != 0;
    for (int f = 0; f < g_recipe.media[m].file_count; ++f)
      if (g_recipe.files[g_recipe.media[m].first_file + f].url[0] != '\0') downloadable = true;
    if (downloadable) {
      g_download_media[g_download_rows++] = m;
      text_copy(g_incoming_names[g_incoming_count++], ENTRY_CAP,
                g_recipe.media[m].download_label[0] ? g_recipe.media[m].download_label : "Download missing game files");
    }
  }
  if (g_svc == NULL || g_svc->file_list_dir == NULL || g_svc->mem_alloc == NULL ||
      g_svc->mem_free == NULL) return 0;
  if (g_svc->file_mkdir != NULL && g_svc->file_mkdir(g_recipe.archive_dir) != 0) {
    pax_file_stat_t directory = {0};
    if (g_svc->file_stat == NULL ||
        g_svc->file_stat(g_recipe.archive_dir, &directory) != 0 || !directory.is_dir)
      return 0;
  }
  char *listing = g_svc->mem_alloc(ZIP_LIST_CAP);
  if (listing == NULL) return 0;
  memset(listing, 0, ZIP_LIST_CAP);
  const int entries = g_svc->file_list_dir(g_recipe.archive_dir, listing, ZIP_LIST_CAP);
  if (entries > 0) {
    char *cursor = listing;
    for (int i = 0; i < entries && g_incoming_count < MAX_INCOMING_FILES; ++i) {
      const size_t remaining = ZIP_LIST_CAP - (size_t)(cursor - listing);
      size_t length = 0;
      while (length < remaining && cursor[length] != '\0') length++;
      if (length == remaining) break;
      const char *name = cursor;
      cursor += length + 1;
      if (length == 0 || strchr(name, '/') != NULL ||
          (!ends_with_ci(name, ".zip") && !ends_with_ci(name, ".iso"))) continue;
      /* ZIPs extracted from a previous attempt are resumable work files, not
       * source discs to offer back to the user. */
      if (strncmp(name, ".disc-installer-cache-", 22) == 0) continue;
      char full_path[PATH_CAP];
      if (!join_path(full_path, sizeof(full_path), g_recipe.archive_dir, name)) continue;
      pax_file_stat_t info = {0};
      if (g_svc->file_stat == NULL || g_svc->file_stat(full_path, &info) != 0 || info.is_dir)
        continue;
      if (text_copy(g_incoming_names[g_incoming_count],
                    sizeof(g_incoming_names[g_incoming_count]), name))
        g_incoming_count++;
    }
  }
  g_svc->mem_free(listing);
  return g_incoming_count;
}

static int incoming_media_index(const char *name)
{
  if (name == NULL) return -1;
  if (ends_with_ci(name, ".zip")) {
    for (int i = 0; i < g_recipe.media_count; ++i)
      if (g_recipe.media[i].zip_name[0] != '\0' &&
          text_equal_ci(name, g_recipe.media[i].zip_name)) return i;
    for (int i = 0; i < g_recipe.media_count; ++i) {
      char zip_alias[NAME_CAP];
      if (g_recipe.media[i].iso_name[0] && text_copy(zip_alias, sizeof(zip_alias), g_recipe.media[i].iso_name)) {
        size_t n = text_len(zip_alias);
        memcpy(zip_alias + n - 3, "zip", 3);
        if (text_equal_ci(name, zip_alias)) return i;
      }
    }
    /* A one-disc recipe can be tried against any disc image in incoming.
     * Sources absent from that disc are skipped, letting another CD supply
     * the remaining outputs on a later selection. */
    return g_recipe.media_count == 1 ? 0 : -1;
  }
  if (!ends_with_ci(name, ".iso")) return -1;
  for (int i = 0; i < g_recipe.media_count; ++i)
    if (g_recipe.media[i].iso_name[0] && text_equal_ci(name, g_recipe.media[i].iso_name)) return i;
  if (g_recipe.media_count == 1) return 0;
  const char *name_end = name + text_len(name);
  for (int i = 0; i < g_recipe.media_count; ++i) {
    const char *zip_name = g_recipe.media[i].zip_name;
    if (!ends_with_ci(zip_name, ".zip")) continue;
    const size_t name_stem = (size_t)(name_end - name) - 4U;
    const size_t zip_stem = text_len(zip_name) - 4U;
    if (name_stem != zip_stem) continue;
    char iso_stem[NAME_CAP], zip_stem_text[NAME_CAP];
    if (name_stem >= sizeof(iso_stem) || zip_stem >= sizeof(zip_stem_text)) continue;
    memcpy(iso_stem, name, name_stem); iso_stem[name_stem] = '\0';
    memcpy(zip_stem_text, zip_name, zip_stem); zip_stem_text[zip_stem] = '\0';
    if (text_equal_ci(iso_stem, zip_stem_text)) return i;
  }
  return -1;
}

static void incoming_button_event(lv_event_t *event)
{
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  const intptr_t item = (intptr_t)lv_event_get_user_data(event);
  if (item < 0) {
    if (g_svc != NULL && g_svc->log_printf != NULL)
      g_svc->log_printf("recipe: touchscreen Back clicked\n");
    g_exit_requested = true;
  } else if (!g_processing_media && item < g_incoming_count) {
    if (g_svc != NULL && g_svc->log_printf != NULL)
      g_svc->log_printf("recipe: touchscreen selected item=%d name=%s\n",
                        (int)item, g_incoming_names[item]);
    g_requested_incoming = (int)item;
  }
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
  (void)indev;
  int x = 0, y = 0;
  const bool pressed = g_svc != NULL && g_svc->touch_read != NULL &&
                       g_svc->touch_read(&x, &y) != 0;
  if (pressed) {
    /* The loader reports touch in its fixed 800x480 framebuffer coordinates;
     * scale to this app's selected canvas (which may be panel-native). */
    data->point.x = (lv_coord_t)((int64_t)x * g_width / TOUCH_FRAME_W);
    data->point.y = (lv_coord_t)((int64_t)y * g_height / TOUCH_FRAME_H);
    if (data->point.x < 0) data->point.x = 0;
    if (data->point.y < 0) data->point.y = 0;
    if (data->point.x >= g_width) data->point.x = g_width - 1;
    if (data->point.y >= g_height) data->point.y = g_height - 1;
    data->state = LV_INDEV_STATE_PRESSED;
    if (!g_touch_was_down && g_svc->log_printf != NULL)
      g_svc->log_printf("recipe: touch press at %d,%d (canvas %d,%d)\n",
                        x, y, (int)data->point.x, (int)data->point.y);
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
  g_touch_was_down = pressed;
}

static int pump_ui_on_host(void *unused)
{
  (void)unused;
  if (g_display == NULL) return -1;
  (void)lv_timer_handler();
  return 0;
}

static int focus_incoming_on_host(void *arg)
{
  const int focus = (int)(intptr_t)arg;
  if (focus < 0 || focus >= g_incoming_count) return 0;
  g_focused_incoming = focus;
  for (int i = 0; i < g_incoming_count; ++i) {
    lv_obj_t *button = g_incoming_buttons[i];
    if (button == NULL) continue;
    lv_obj_set_style_border_color(button, lv_color_hex(i == focus ? COL_ACCENT : COL_MUTED),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_width(button, i == focus ? 3 : 1, LV_PART_MAIN);
  }
  if (g_incoming_buttons[focus] != NULL)
    lv_obj_scroll_to_view(g_incoming_buttons[focus], LV_ANIM_OFF);
  return 0;
}

static int set_processing_ui_on_host(void *unused)
{
  (void)unused;
  if (g_incoming_list != NULL) {
    if (g_processing_media) lv_obj_add_flag(g_incoming_list, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(g_incoming_list, LV_OBJ_FLAG_HIDDEN);
  }
  if (g_empty_label != NULL) {
    if (g_processing_media || g_incoming_count > 0)
      lv_obj_add_flag(g_empty_label, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_clear_flag(g_empty_label, LV_OBJ_FLAG_HIDDEN);
  }
  if (g_progress_bar != NULL) {
    if (g_processing_media) lv_obj_clear_flag(g_progress_bar, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(g_progress_bar, LV_OBJ_FLAG_HIDDEN);
  }
  if (g_progress_label != NULL) {
    if (g_processing_media) lv_obj_clear_flag(g_progress_label, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(g_progress_label, LV_OBJ_FLAG_HIDDEN);
  }
  if (g_status_label != NULL) lv_label_set_text(g_status_label, g_status);
  if (g_back_button != NULL) {
    if (g_processing_media) lv_obj_add_flag(g_back_button, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(g_back_button, LV_OBJ_FLAG_HIDDEN);
  }
  lv_timer_handler();
  return 0;
}

static void report(const char *status, uint32_t done, uint32_t total, uint32_t color);

static int tick_cb(void)
{
  return (int)(g_svc->get_time_us() / 1000);
}

static void display_flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
  (void)area;
  g_svc->display_write_frame_custom((const uint16_t *)pixels,
                                    (uint16_t)g_width, (uint16_t)g_height, 1.0f, false);
  lv_display_flush_ready(display);
}

static int build_ui_on_host(void *unused)
{
  (void)unused;
  lv_init();
  lv_tick_set_cb((lv_tick_get_cb_t)tick_cb);
  g_display = lv_display_create(g_width, g_height);
  if (g_display == NULL) return -1;
  lv_display_set_default(g_display);
  lv_display_set_flush_cb(g_display, display_flush_cb);
  lv_display_set_buffers(g_display, g_canvas, NULL,
                         (uint32_t)g_width * (uint32_t)g_height * sizeof(uint16_t),
                         LV_DISPLAY_RENDER_MODE_FULL);
  g_touch_indev = NULL;
  if (g_svc != NULL && g_svc->touch_read != NULL) {
    g_touch_indev = lv_indev_create();
    if (g_touch_indev != NULL) {
      lv_indev_set_type(g_touch_indev, LV_INDEV_TYPE_POINTER);
      lv_indev_set_read_cb(g_touch_indev, touch_read_cb);
      lv_indev_set_display(g_touch_indev, g_display);
    }
  }
  if (g_svc != NULL && g_svc->log_printf != NULL)
    g_svc->log_printf("recipe: touch input %s; canvas=%dx%d\n",
                      g_touch_indev != NULL ? "ready" : "unavailable",
                      g_width, g_height);
  lv_obj_t *screen = lv_display_get_screen_active(g_display);
  lv_obj_remove_style_all(screen);
  lv_obj_set_style_bg_color(screen, lv_color_hex(COL_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_t *panel = lv_obj_create(screen);
  lv_obj_set_size(panel, g_width - 32, g_height - 32);
  lv_obj_center(panel);
  lv_obj_set_style_bg_color(panel, lv_color_hex(COL_PANEL), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(panel, LV_OPA_90, LV_PART_MAIN);
  lv_obj_set_style_border_color(panel, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
  lv_obj_set_style_border_width(panel, 2, LV_PART_MAIN);
  lv_obj_set_style_radius(panel, 14, LV_PART_MAIN);
  lv_obj_t *title = lv_label_create(panel);
  lv_label_set_text(title, "GET GAME DATA");
  lv_obj_set_style_text_color(title, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 20, 16);
  g_back_button = lv_button_create(panel);
  lv_obj_set_size(g_back_button, 90, 36);
  lv_obj_align(g_back_button, LV_ALIGN_TOP_RIGHT, -16, 8);
  lv_obj_set_style_bg_color(g_back_button, lv_color_hex(0x173758), LV_PART_MAIN);
  lv_obj_set_style_border_color(g_back_button, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
  lv_obj_set_style_border_width(g_back_button, 1, LV_PART_MAIN);
  lv_obj_t *back_label = lv_label_create(g_back_button);
  lv_label_set_text(back_label, "BACK");
  lv_obj_set_style_text_color(back_label, lv_color_hex(COL_TEXT), LV_PART_MAIN);
  lv_obj_center(back_label);
  lv_obj_add_event_cb(g_back_button, incoming_button_event, LV_EVENT_CLICKED,
                      (void *)(intptr_t)-1);
  g_status_label = lv_label_create(panel);
  lv_obj_set_width(g_status_label, g_width - 104);
  lv_label_set_long_mode(g_status_label, LV_LABEL_LONG_WRAP);
  lv_label_set_text(g_status_label, g_status);
  lv_obj_set_style_text_color(g_status_label, lv_color_hex(COL_TEXT), LV_PART_MAIN);
  lv_obj_align(g_status_label, LV_ALIGN_TOP_LEFT, 20, 52);
  g_incoming_list = lv_obj_create(panel);
  lv_obj_set_pos(g_incoming_list, 20, 88);
  lv_obj_set_size(g_incoming_list, g_width - 72, g_height - 176);
  lv_obj_set_style_bg_color(g_incoming_list, lv_color_hex(0x081A2B), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(g_incoming_list, LV_OPA_70, LV_PART_MAIN);
  lv_obj_set_style_border_color(g_incoming_list, lv_color_hex(0x315776), LV_PART_MAIN);
  lv_obj_set_style_border_width(g_incoming_list, 1, LV_PART_MAIN);
  lv_obj_set_style_pad_all(g_incoming_list, 8, LV_PART_MAIN);
  lv_obj_set_scroll_dir(g_incoming_list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(g_incoming_list, LV_SCROLLBAR_MODE_AUTO);
  memset(g_incoming_buttons, 0, sizeof(g_incoming_buttons));
  for (int i = 0; i < g_incoming_count; ++i) {
    lv_obj_t *button = lv_button_create(g_incoming_list);
    g_incoming_buttons[i] = button;
    lv_obj_set_size(button, g_width - 106, 38);
    lv_obj_set_pos(button, 8, 8 + i * 46);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x153554), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lv_color_hex(i == 0 ? COL_ACCENT : COL_MUTED),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_width(button, i == 0 ? 3 : 1, LV_PART_MAIN);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, g_incoming_names[i]);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, g_width - 136);
    lv_obj_set_style_text_color(label, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_center(label);
    lv_obj_add_event_cb(button, incoming_button_event, LV_EVENT_CLICKED,
                        (void *)(intptr_t)i);
  }
  g_empty_label = lv_label_create(g_incoming_list);
  char empty_text[PATH_CAP + 64] = "No disc images found. Copy .ZIP or .ISO files into:\n";
  text_append(empty_text, sizeof(empty_text), g_recipe.archive_dir);
  lv_label_set_text(g_empty_label, empty_text);
  lv_obj_set_style_text_color(g_empty_label, lv_color_hex(COL_MUTED), LV_PART_MAIN);
  lv_obj_center(g_empty_label);
  g_progress_bar = lv_bar_create(panel);
  lv_obj_set_size(g_progress_bar, g_width - 96, 18);
  lv_obj_align(g_progress_bar, LV_ALIGN_TOP_LEFT, 20, g_height / 2);
  lv_bar_set_range(g_progress_bar, 0, 100);
  lv_bar_set_value(g_progress_bar, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(g_progress_bar, lv_color_hex(0x203C59), LV_PART_MAIN);
  lv_obj_set_style_bg_color(g_progress_bar, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
  g_progress_label = lv_label_create(panel);
  lv_obj_set_style_text_color(g_progress_label, lv_color_hex(COL_MUTED), LV_PART_MAIN);
  lv_obj_align(g_progress_label, LV_ALIGN_TOP_LEFT, 20, g_height / 2 + 30);
  lv_label_set_text(g_progress_label, "0 / 0 files");
  lv_obj_t *hint = lv_label_create(panel);
  lv_label_set_text(hint, "Choose a download or a local disc. Existing files are kept. B returns.");
  lv_obj_set_style_text_color(hint, lv_color_hex(COL_MUTED), LV_PART_MAIN);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 20, -14);
  g_processing_media = false;
  if (g_progress_bar != NULL) lv_obj_add_flag(g_progress_bar, LV_OBJ_FLAG_HIDDEN);
  if (g_progress_label != NULL) lv_obj_add_flag(g_progress_label, LV_OBJ_FLAG_HIDDEN);
  if (g_incoming_count > 0) lv_obj_add_flag(g_empty_label, LV_OBJ_FLAG_HIDDEN);
  lv_timer_handler();
  return 0;
}

static int show_instructions_on_host(void *unused)
{
  (void)unused;
  if (g_instructions_overlay) lv_obj_delete(g_instructions_overlay);
  g_instructions_overlay = lv_obj_create(lv_display_get_screen_active(g_display));
  lv_obj_set_size(g_instructions_overlay, g_width, g_height);
  lv_obj_set_style_bg_color(g_instructions_overlay, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_bg_opa(g_instructions_overlay, LV_OPA_COVER, 0);
  lv_obj_set_scroll_dir(g_instructions_overlay, LV_DIR_VER);
  lv_obj_t *label=lv_label_create(g_instructions_overlay);
  lv_obj_set_width(label,g_width-60);
  lv_label_set_long_mode(label,LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_color(label,lv_color_hex(COL_TEXT),0);
  lv_label_set_text(label,g_recipe.instructions);
  lv_obj_t *back=lv_button_create(g_instructions_overlay);
  lv_obj_set_pos(back,g_width-160,0);
  lv_obj_t *text=lv_label_create(back);lv_label_set_text(text,"BACK");
  lv_obj_add_event_cb(back,incoming_button_event,LV_EVENT_CLICKED,(void *)(intptr_t)-1);
  lv_obj_set_y(label,55);
  return 0;
}

static int scroll_instructions_on_host(void *direction)
{
  if (g_instructions_overlay != NULL)
    lv_obj_scroll_by(g_instructions_overlay, 0, (int)(intptr_t)direction, LV_ANIM_OFF);
  return 0;
}

static int update_ui_on_host(void *unused)
{
  (void)unused;
  if (g_status_label != NULL) {
    lv_label_set_text(g_status_label, g_status);
    lv_obj_set_style_text_color(g_status_label,
        lv_color_hex(g_success ? COL_OK : COL_TEXT), LV_PART_MAIN);
  }
  if (g_progress_bar != NULL) {
    const int pct = g_byte_progress_active && g_byte_total != 0 ?
      (int)((g_byte_done * 100U) / g_byte_total) :
      (g_total == 0 ? 0 : (int)(((uint64_t)g_done * 100U) / g_total));
    lv_bar_set_value(g_progress_bar, pct > 100 ? 100 : pct, LV_ANIM_OFF);
  }
  if (g_progress_label != NULL) {
    char count[112];
    count[0] = '\0';
    if (g_byte_progress_active) {
      (void)text_append(count, sizeof(count), "file ");
      (void)append_u32(count, sizeof(count), g_done + 1U);
      (void)text_append(count, sizeof(count), "/");
      (void)append_u32(count, sizeof(count), g_total);
      (void)text_append(count, sizeof(count), "   ");
      append_mib_tenths(count, sizeof(count), g_byte_done);
      (void)text_append(count, sizeof(count), " / ");
      append_mib_tenths(count, sizeof(count), g_byte_total);
      (void)text_append(count, sizeof(count), " MB   ");
      if (g_byte_rate_bps >= 1024U * 1024U) {
        append_mib_tenths(count, sizeof(count), g_byte_rate_bps);
        (void)text_append(count, sizeof(count), " MB/s");
      } else {
        (void)append_u32(count, sizeof(count), (uint32_t)(g_byte_rate_bps / 1024U));
        (void)text_append(count, sizeof(count), " KB/s");
      }
    } else {
      (void)append_u32(count, sizeof(count), g_done);
      (void)text_append(count, sizeof(count), " / ");
      (void)append_u32(count, sizeof(count), g_total);
      (void)text_append(count, sizeof(count), " files");
    }
    lv_label_set_text(g_progress_label, count);
  }
  lv_timer_handler();
  return 0;
}

static void report(const char *status, uint32_t done, uint32_t total, uint32_t color)
{
  (void)color;
  (void)text_copy(g_status, sizeof(g_status), status);
  g_done = done;
  g_total = total;
  if (g_svc->log_printf != NULL) g_svc->log_printf("recipe: %s (%u/%u)\n", g_status, done, total);
  if (g_svc->ui_call_sync != NULL)
    (void)g_svc->ui_call_sync(update_ui_on_host, NULL, 1500);
}

static bool parse_decimal(const char **cursor, unsigned max, unsigned *out)
{
  if (cursor == NULL || *cursor == NULL || **cursor < '0' || **cursor > '9') return false;
  unsigned value = 0;
  while (**cursor >= '0' && **cursor <= '9') {
    const unsigned d = (unsigned)(*(*cursor)++ - '0');
    if (value > (max - d) / 10U) return false;
    value = value * 10U + d;
  }
  *out = value;
  return true;
}

static bool parse_cue_layout(const char *cue_path, const char *cue_entry,
                             char *bin_entry, size_t bin_cap,
                             iso9660_layout_t *layout)
{
  void *f = g_svc->file_open(cue_path, "rb");
  if (f == NULL) return false;
  char *cue = g_svc->mem_alloc(8192);
  if (cue == NULL) {
    (void)g_svc->file_close(f);
    return false;
  }
  memset(cue, 0, 8192);
  bool result = false;
  const size_t bytes = g_svc->file_read(cue, 1, 8191, f);
  char extra;
  const bool too_large = g_svc->file_read(&extra, 1, 1, f) != 0;
  (void)g_svc->file_close(f);
  if (too_large || bytes == 0) goto cleanup;
  cue[bytes] = '\0';
  char current[160] = "", bin[160] = "";
  bool track = false, found_index = false;
  uint16_t stride = 0, payload = 0;
  uint64_t offset = 0;
  char *line = cue;
  while (*line != '\0') {
    char *end = line;
    while (*end && *end != '\n' && *end != '\r') end++;
    const char saved = *end; *end = '\0';
    while (*line == ' ' || *line == '\t') line++;
    const char *p = line;
    if ((p[0] == 'F' || p[0] == 'f') && (p[1] == 'I' || p[1] == 'i') &&
        (p[2] == 'L' || p[2] == 'l') && (p[3] == 'E' || p[3] == 'e') &&
        (p[4] == ' ' || p[4] == '\t')) {
      p += 5; while (*p == ' ' || *p == '\t') p++;
      char name[160]; size_t n = 0;
      if (*p == '"') { p++; while (*p && *p != '"' && n + 1 < sizeof(name)) name[n++] = *p++; }
      else while (*p && *p != ' ' && *p != '\t' && n + 1 < sizeof(name)) name[n++] = *p++;
      name[n] = '\0'; while (*p == ' ' || *p == '\t') p++;
      if (n && relative_path_valid(name) && strncmp(p, "BINARY", 6) == 0)
        text_copy(current, sizeof(current), name);
      else current[0] = '\0';
      track = false;
    } else if ((p[0] == 'T' || p[0] == 't') && strncmp(p + 1, "RACK", 4) == 0 &&
               (p[5] == ' ' || p[5] == '\t')) {
      p += 6; while (*p == ' ' || *p == '\t') p++;
      unsigned number = 0;
      if (parse_decimal(&p, 99, &number) && number > 0) {
        while (*p == ' ' || *p == '\t') p++;
        if ((p[0] == 'M' || p[0] == 'm') && strncmp(p + 1, "ODE1/", 5) == 0) {
          p += 6; unsigned size = 0;
          if (parse_decimal(&p, 2352, &size) && current[0] &&
              (size == 2048 || size == 2352)) {
            track = true; stride = (uint16_t)size;
            payload = size == 2352 ? 16 : 0;
            text_copy(bin, sizeof(bin), current);
          }
        }
      }
    } else if (track && (p[0] == 'I' || p[0] == 'i') && strncmp(p + 1, "NDEX", 4) == 0 &&
               (p[5] == ' ' || p[5] == '\t')) {
      p += 6; while (*p == ' ' || *p == '\t') p++;
      unsigned index = 0, min = 0, sec = 0, frame = 0;
      if (parse_decimal(&p, 99, &index) && index == 1 && (*p == ' ' || *p == '\t')) {
        while (*p == ' ' || *p == '\t') p++;
        if (parse_decimal(&p, 999, &min) && *p++ == ':' &&
            parse_decimal(&p, 59, &sec) && *p++ == ':' && parse_decimal(&p, 74, &frame)) {
          offset = (((uint64_t)min * 60U + sec) * 75U + frame) * stride;
          found_index = true; *end = saved; break;
        }
      }
    }
    *end = saved;
    if (!saved) break;
    line = end + 1;
    if (saved == '\r' && *line == '\n') line++;
  }
  if (!found_index || bin[0] == '\0' || stride == 0) goto cleanup;
  const char *slash = strrchr(cue_entry, '/');
  const size_t prefix = slash != NULL ? (size_t)(slash - cue_entry + 1) : 0;
  if (prefix + text_len(bin) + 1 > bin_cap) goto cleanup;
  if (prefix) memcpy(bin_entry, cue_entry, prefix);
  text_copy(bin_entry + prefix, bin_cap - prefix, bin);
  if (!relative_path_valid(bin_entry)) goto cleanup;
  layout->track_offset = offset;
  layout->sector_size = stride;
  layout->user_data_offset = payload;
  result = true;
cleanup:
  g_svc->mem_free(cue);
  return result;
}

typedef struct { const app_services_t *svc; void *file; } io_ctx_t;
static int io_seek(void *ctx, long offset, int origin) { io_ctx_t *c = ctx; return c->svc->file_seek(c->file, offset, origin); }
static long io_tell(void *ctx) { io_ctx_t *c = ctx; return c->svc->file_tell(c->file); }
static size_t io_read(void *buf, size_t size, size_t count, void *ctx) { io_ctx_t *c = ctx; return c->svc->file_read(buf, size, count, c->file); }
static size_t io_write(const void *buf, size_t size, size_t count, void *ctx) { io_ctx_t *c = ctx; return c->svc->file_write(buf, size, count, c->file); }

static void iso_copy_progress(void *context, uint64_t copied, uint64_t total)
{
  (void)context;
  g_byte_done = copied;
  g_byte_total = total;
  const int64_t now = g_svc->get_time_us != NULL ? g_svc->get_time_us() : 0;
  if (now > 0) {
    const uint64_t now_us = (uint64_t)now;
    if (g_progress_sample_us == 0) {
      g_progress_sample_us = now_us;
      g_progress_sample_bytes = copied;
    } else if (now_us - g_progress_sample_us >= 500000U) {
      const uint64_t delta = copied >= g_progress_sample_bytes ? copied - g_progress_sample_bytes : 0;
      g_byte_rate_bps = delta * 1000000U / (now_us - g_progress_sample_us);
      g_progress_sample_us = now_us;
      g_progress_sample_bytes = copied;
    }
    if (g_last_progress_log_us == 0 || now_us - g_last_progress_log_us >= 1000000U || copied >= total) {
      g_last_progress_log_us = now_us;
      if (g_svc->log_printf != NULL)
        g_svc->log_printf("recipe: progress %llu/%llu bytes at %llu B/s\n",
          (unsigned long long)copied, (unsigned long long)total,
          (unsigned long long)g_byte_rate_bps);
    }
    if (copied < total && g_last_progress_ui_us != 0 &&
        now_us - g_last_progress_ui_us < 150000U) return;
    g_last_progress_ui_us = now_us;
  }
  if (g_svc->ui_call_sync != NULL)
    (void)g_svc->ui_call_sync(update_ui_on_host, NULL, 1500);
}

static void zip_extract_progress(void *context, uint64_t copied, uint64_t total)
{
  iso_copy_progress(context, copied, total);
}

static zip_extract_named_fn get_named_zip_service(void)
{
  return g_svc != NULL ? g_svc->file_zip_extract_named : NULL;
}

static uint32_t archive_key(const char *name)
{
  uint32_t hash = 2166136261U;
  for (const unsigned char *p = (const unsigned char *)name; *p != 0; ++p) {
    unsigned char c = *p;
    if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + ('a' - 'A'));
    hash = (hash ^ c) * 16777619U;
  }
  return hash;
}

static bool make_cache_stem(const char *archive_name, char *out, size_t cap)
{
  static const char hex[] = "0123456789abcdef";
  if (!text_copy(out, cap, ".disc-installer-cache-")) return false;
  const uint32_t hash = archive_key(archive_name);
  for (int shift = 28; shift >= 0; shift -= 4) {
    char digit[2] = {hex[(hash >> shift) & 0x0fU], '\0'};
    if (!text_append(out, cap, digit)) return false;
  }
  return true;
}

static bool stage_media(const recipe_media_t *media, const char *archive,
                        const char *archive_name, char *image_path,
                        size_t image_cap, iso9660_layout_t *layout)
{
  zip_extract_named_fn extract = media->auto_entry ? NULL : get_named_zip_service();
  const bool can_extract_first = media->auto_entry && g_svc->file_zip_extract_first != NULL;
  if ((!can_extract_first && extract == NULL) || g_svc->file_remove == NULL ||
      g_svc->file_mkdir == NULL || archive == NULL || archive_name == NULL ||
      !ends_with_ci(archive_name, ".zip")) return false;
  if (g_svc->file_mkdir(g_recipe.archive_dir) != 0) {
    pax_file_stat_t dir = {0};
    if (g_svc->file_stat == NULL || g_svc->file_stat(g_recipe.archive_dir, &dir) != 0 || !dir.is_dir)
      return false;
  }

  char cache_stem[48];
  if (!make_cache_stem(archive_name, cache_stem, sizeof(cache_stem))) return false;
  const bool zip_iso = strcmp(media->format, "zip-iso9660") == 0;
  char cue_path[PATH_CAP] = "", bin_path[PATH_CAP] = "";
  if (zip_iso) {
    if (!join_path(image_path, image_cap, g_recipe.archive_dir, cache_stem) ||
        !text_append(image_path, image_cap, ".iso")) return false;
  } else {
    if (!join_path(cue_path, sizeof(cue_path), g_recipe.archive_dir, cache_stem) ||
        !text_append(cue_path, sizeof(cue_path), ".cue") ||
        !join_path(bin_path, sizeof(bin_path), g_recipe.archive_dir, cache_stem) ||
        !text_append(bin_path, sizeof(bin_path), ".bin") ||
        !text_copy(image_path, image_cap, bin_path)) return false;
  }

  if (zip_iso && media->auto_entry) {
    uint64_t cached_size = 0;
    if (cached_iso_is_valid(image_path, &cached_size)) {
      text_copy(g_status, sizeof(g_status), "Using the staged ISO; resuming missing-file extraction...");
      report(g_status, g_done, g_total, COL_TEXT);
      if (g_svc->log_printf != NULL)
        g_svc->log_printf("recipe: reusing cached ISO %s (%llu bytes) for %s\n",
          image_path, (unsigned long long)cached_size, archive);
      *layout = (iso9660_layout_t){0, 2048, 0};
      return true;
    }
  }

  char status[STATUS_CAP] = "Reading ";
  (void)text_append(status, sizeof(status), archive_name);
  (void)text_append(status, sizeof(status), " and staging its disc image...");
  report(status, g_done, g_total, COL_TEXT);
  uint32_t written = 0;
  if (zip_iso && media->auto_entry) {
    (void)g_svc->file_remove(image_path);
    g_byte_progress_active = g_svc->file_zip_extract_first_progress != NULL;
    g_byte_done = 0;
    g_byte_total = 0;
    g_byte_rate_bps = 0;
    g_last_progress_ui_us = 0;
    g_last_progress_log_us = 0;
    g_progress_sample_us = 0;
    g_progress_sample_bytes = 0;
    int zip_result;
    if (g_svc->file_zip_extract_first_progress != NULL) {
      zip_result = g_svc->file_zip_extract_first_progress(
          archive, ".iso|", g_recipe.archive_dir, cache_stem,
          image_path, image_cap, zip_extract_progress, NULL);
    } else {
      zip_result = g_svc->file_zip_extract_first(
          archive, ".iso|", g_recipe.archive_dir, cache_stem,
          image_path, image_cap);
    }
    g_byte_progress_active = false;
    if (g_svc->ui_call_sync != NULL)
      (void)g_svc->ui_call_sync(update_ui_on_host, NULL, 1500);
    if (zip_result == 0 && cached_iso_is_valid(image_path, NULL)) {
      *layout = (iso9660_layout_t){0, 2048, 0};
      return true;
    }
    (void)g_svc->file_remove(image_path);
    return false;
  }

  (void)g_svc->file_remove(cue_path);
  (void)g_svc->file_remove(bin_path);
  if (zip_iso) {
    if (extract(archive, media->entry, image_path, MAX_IMAGE_BYTES, &written) == 0 && written > 0) {
      *layout = (iso9660_layout_t){0, 2048, 0};
      return true;
    }
  } else {
    if (extract(archive, media->entry, cue_path, 64U * 1024U, &written) == 0 && written > 0) {
      char bin_entry[ENTRY_CAP];
      iso9660_layout_t cue_layout = {0};
      if (parse_cue_layout(cue_path, media->entry, bin_entry, sizeof(bin_entry), &cue_layout) &&
          extract(archive, bin_entry, bin_path, MAX_IMAGE_BYTES, &written) == 0 && written > 0) {
        (void)g_svc->file_remove(cue_path);
        *layout = cue_layout;
        return true;
      }
    }
  }
  (void)g_svc->file_remove(cue_path);
  (void)g_svc->file_remove(bin_path);
  return false;
}

static int extract_one(const char *image_path, const char *source, const char *target,
                       const iso9660_layout_t *layout)
{
  if (file_exists(target)) return 1;
  if (!path_under_roms(target) || !make_parent_dirs(target)) {
    if (g_svc->log_printf != NULL)
      g_svc->log_printf("recipe: cannot prepare target folder for %s\n", target);
    return -1;
  }
  char temp[PATH_CAP];
  if (!text_copy(temp, sizeof(temp), target) || !text_append(temp, sizeof(temp), ".disc-tmp")) {
    if (g_svc->log_printf != NULL)
      g_svc->log_printf("recipe: temporary output path is too long for %s\n", target);
    return -2;
  }
  (void)g_svc->file_remove(temp);
  void *input = g_svc->file_open(image_path, "rb");
  if (input == NULL || g_svc->file_seek(input, 0, 2) != 0) {
    if (input) (void)g_svc->file_close(input);
    if (g_svc->log_printf != NULL)
      g_svc->log_printf("recipe: cannot open/seek staged image %s\n", image_path);
    return -3;
  }
  const long size = g_svc->file_tell(input);
  if (size <= 0 || g_svc->file_seek(input, 0, 0) != 0) {
    (void)g_svc->file_close(input);
    if (g_svc->log_printf != NULL)
      g_svc->log_printf("recipe: invalid staged image size %ld for %s\n", size, image_path);
    return -3;
  }
  void *output = g_svc->file_open(temp, "wb");
  if (output == NULL) {
    (void)g_svc->file_close(input);
    if (g_svc->log_printf != NULL)
      g_svc->log_printf("recipe: cannot create temporary output %s\n", temp);
    return -4;
  }
  io_ctx_t in_ctx = {g_svc, input}, out_ctx = {g_svc, output};
  const iso9660_io_t in_io = {&in_ctx, io_seek, io_tell, io_read, io_write};
  const iso9660_io_t out_io = {&out_ctx, io_seek, io_tell, io_read, io_write};
  uint64_t copied = 0;
  uint64_t source_bytes = 0;
  const int result = iso9660_copy_file_with_progress(&in_io, &out_io, source,
      layout, size, MAX_FILE_BYTES, &copied, &source_bytes, iso_copy_progress, NULL);
  const int out_closed = g_svc->file_close(output);
  (void)g_svc->file_close(input);
  if (result != 0 || out_closed != 0 || copied == 0) {
    if (g_svc->log_printf != NULL)
      g_svc->log_printf("recipe: ISO copy failed source=%s image=%s image_bytes=%ld iso_result=%d source_bytes=%llu limit=%u copied=%llu close_result=%d\n",
        source, image_path, size, result, (unsigned long long)source_bytes,
        (unsigned)MAX_FILE_BYTES, (unsigned long long)copied, out_closed);
    (void)g_svc->file_remove(temp);
    return result != 0 ? -100 + result : -5;
  }
  if (file_exists(target) || g_svc->file_rename(temp, target) != 0) {
    const bool target_exists = file_exists(target);
    if (g_svc->log_printf != NULL && !target_exists)
      g_svc->log_printf("recipe: cannot commit extracted file %s -> %s\n", temp, target);
    (void)g_svc->file_remove(temp);
    return target_exists ? 1 : -6;
  }
  return 0;
}

static bool required_files_ready(void)
{
  for (int i = 0; i < g_recipe.file_count; ++i)
    if (!g_recipe.files[i].optional && !file_exists(g_recipe.files[i].target)) return false;
  return true;
}

static void remove_staged_cache_files(void)
{
  if (g_svc->file_list_dir == NULL || g_svc->file_remove == NULL ||
      g_svc->mem_alloc == NULL || g_svc->mem_free == NULL) return;
  char *listing = g_svc->mem_alloc(ZIP_LIST_CAP);
  if (listing == NULL) return;
  memset(listing, 0, ZIP_LIST_CAP);
  const int entries = g_svc->file_list_dir(g_recipe.archive_dir, listing, ZIP_LIST_CAP);
  char *cursor = listing;
  for (int i = 0; i < entries; ++i) {
    const size_t remaining = ZIP_LIST_CAP - (size_t)(cursor - listing);
    size_t length = 0;
    while (length < remaining && cursor[length] != '\0') length++;
    if (length == remaining) break;
    const char *name = cursor;
    cursor += length + 1;
    if (strncmp(name, ".disc-installer-cache-", 22) != 0) continue;
    char path[PATH_CAP];
    if (join_path(path, sizeof(path), g_recipe.archive_dir, name))
      (void)g_svc->file_remove(path);
  }
  g_svc->mem_free(listing);
}

/* The firmware streams to a temporary file and commits on HTTP success.
 * Keep existing files; check the published member size before accepting a new one.
 * The UI stays responsive, but we finish an in-flight transfer before closing. */
static bool verify_sha256(const char *path,const char *expected)
{
  if (!expected[0]) return true;
  if (strlen(expected)!=64 || !g_svc->file_open || !g_svc->file_read || !g_svc->file_close) return false;
  void *f=g_svc->file_open(path,"rb");if(!f)return false;
  recipe_sha256_t sha;recipe_sha256_init(&sha);uint8_t buffer[4096];size_t n;
  uint64_t last=0;
  while((n=g_svc->file_read(buffer,1,sizeof(buffer),f))!=0){recipe_sha256_update(&sha,buffer,n);
    uint64_t now=g_svc->get_time_us();if(now-last>250000){(void)g_svc->ui_call_sync(pump_ui_on_host,NULL,1000);last=now;}}
  (void)g_svc->file_close(f);char actual[65];recipe_sha256_final(&sha,actual);return strcmp(actual,expected)==0;
}

static bool download_one(recipe_file_t *mapping,uint32_t done,uint32_t total)
{
  pax_file_stat_t existing={0};
  if(g_svc->file_stat(mapping->target,&existing)==0){
    if(existing.is_dir || existing.size==0){report("An empty file or directory occupies the target. Remove it before retrying.",done,total,COL_WARN);return false;}
    report("Keeping an existing file.",done,total,COL_TEXT);return true;
  }
  if(!make_parent_dirs(mapping->target) || g_svc->archive_download_start(mapping->url,mapping->target)!=0){report("Could not start download. Retry or use local files.",done,total,COL_WARN);return false;}
  bool complete=false;uint64_t last=0;
  for(;;){pax_install_progress_t progress={0};
    if(g_svc->store_install_status(&progress)!=0){g_svc->delay_ms(100);continue;}
    if(progress.state==2 || progress.state==3){complete=progress.state==2;if(!complete)report(progress.status[0]?progress.status:"Download failed; local files remain available.",done,total,COL_WARN);break;}
    uint64_t now=g_svc->get_time_us();if(now-last>=250000){char status[STATUS_CAP]="Downloading ";text_append(status,sizeof(status),mapping->source);g_byte_progress_active=true;g_byte_done=progress.done;g_byte_total=mapping->expected_size;report(status,done,total,COL_TEXT);last=now;}
    (void)g_svc->ui_call_sync(pump_ui_on_host,NULL,1000);g_svc->delay_ms(50);
  }
  g_byte_progress_active=false;if(!complete)return false;
  pax_file_stat_t received={0};
  if(g_svc->file_stat(mapping->target,&received)!=0 || received.is_dir || received.size!=mapping->expected_size || !verify_sha256(mapping->target,mapping->sha256)){
    (void)g_svc->file_remove(mapping->target);report("Download size or checksum did not match. Retry or use local files.",done,total,COL_ERROR);return false;}
  return true;
}

static bool extract_zip_media(recipe_media_t *media,const char *archive)
{
  if(!g_svc->file_zip_extract_named){report("This firmware cannot extract named ZIP files.",0,0,COL_WARN);return false;}
  if(!verify_sha256(archive,media->archive_sha256)){report("ZIP checksum does not match; replace the cached archive and retry.",0,0,COL_ERROR);return false;}
  unsigned done=0;
  for(int i=0;i<media->file_count && !g_exit_requested;i++){
    recipe_file_t *f=&g_recipe.files[media->first_file+i];
    if(file_exists(f->target)){done++;continue;}
    pax_file_stat_t occupied={0};
    if(g_svc->file_stat(f->target,&occupied)==0){
      report("An empty file or directory occupies the target. Remove it before retrying.",done,media->file_count,COL_WARN);return false;
    }
    if(!make_parent_dirs(f->target))return false;
    report("Installing ZIP contents...",done,media->file_count,COL_TEXT);uint32_t written=0;
    if(g_svc->file_zip_extract_named(archive,f->source,f->target,MAX_FILE_BYTES,&written)!=0){report("ZIP entry could not be extracted; existing files were kept.",done,media->file_count,COL_ERROR);return false;}
    if((f->expected_size && written!=f->expected_size) || !verify_sha256(f->target,f->sha256)){
      (void)g_svc->file_remove(f->target);report("Extracted file failed size or checksum verification.",done,media->file_count,COL_ERROR);return false;}
    done++;
  }
  report("ZIP files installed. Existing files were kept.",done,media->file_count,COL_OK);return done==(unsigned)media->file_count;
}

static bool download_media(int media_index)
{
  if(!g_svc->archive_download_start || !g_svc->store_install_status || !g_svc->file_stat || !g_svc->file_remove){report("Update firmware for downloads; local-file setup remains available.",0,0,COL_WARN);return false;}
  recipe_media_t *media=&g_recipe.media[media_index];
  if(media->archive_url[0]){
    recipe_file_t archive={0};text_copy(archive.source,sizeof(archive.source),media->zip_name);
    text_copy(archive.url,sizeof(archive.url),media->archive_url);text_copy(archive.sha256,sizeof(archive.sha256),media->archive_sha256);
    archive.expected_size=media->archive_size;
    if(!join_path(archive.target,sizeof(archive.target),g_recipe.archive_dir,media->zip_name) || !download_one(&archive,0,1))return false;
    return extract_zip_media(media,archive.target);
  }
  unsigned total=0,done=0;for(int i=0;i<media->file_count;i++)if(g_recipe.files[media->first_file+i].url[0])total++;
  for(int i=0;i<media->file_count && !g_exit_requested;i++){
    recipe_file_t *f=&g_recipe.files[media->first_file+i];if(!f->url[0])continue;
    if(!download_one(f,done,total))return false;done++;
  }
  report("Files downloaded; existing files were kept.",done,total,COL_OK);return done==total;
}

static bool prepare_folders(void)
{
  if(!g_svc->file_mkdir)return false;
  for(int i=0;i<g_recipe.folder_count;i++){
    char marker[PATH_CAP];if(!join_path(marker,sizeof(marker),g_recipe.folders[i],".pax-folder-marker") || !make_parent_dirs(marker))return false;
  }
  (void)g_svc->ui_call_sync(show_instructions_on_host,NULL,1500);return true;
}

static bool run_recipe_for_incoming(int incoming_index)
{
  if (g_svc->file_open == NULL || g_svc->file_close == NULL || g_svc->file_read == NULL ||
      g_svc->file_seek == NULL || g_svc->file_tell == NULL || g_svc->file_write == NULL ||
      g_svc->file_rename == NULL || g_svc->file_stat == NULL) {
    text_copy(g_status, sizeof(g_status), "This firmware is missing required file services.");
    return false;
  }
  if (incoming_index < 0 || incoming_index >= g_incoming_count) return false;
  if (incoming_index < g_download_rows) return g_download_media[incoming_index]<0 ? prepare_folders() : download_media(g_download_media[incoming_index]);
  const char *archive_name = g_incoming_names[incoming_index];
  const int media_index = incoming_media_index(archive_name);
  if (media_index < 0 || media_index >= g_recipe.media_count) {
    text_copy(g_status, sizeof(g_status), "No extraction recipe matches this disc image.");
    report(g_status, 0, 0, COL_WARN);
    return false;
  }
  recipe_media_t *media = &g_recipe.media[media_index];
  char archive[PATH_CAP];
  if (!join_path(archive, sizeof(archive), g_recipe.archive_dir, archive_name)) {
    text_copy(g_status, sizeof(g_status), "The selected file path is too long.");
    report(g_status, 0, 0, COL_ERROR);
    return false;
  }

  if (strcmp(media->format,"zip-files")==0) return extract_zip_media(media,archive);
  g_total = (uint32_t)media->file_count;
  g_done = 0;
  char heading[STATUS_CAP] = "Checking ";
  (void)text_append(heading, sizeof(heading), archive_name);
  (void)text_append(heading, sizeof(heading), " for missing recipe files...");
  report(heading, 0, g_total, COL_TEXT);

  bool has_missing_target = false;
  for (int i = 0; i < media->file_count; ++i) {
    recipe_file_t *recipe_file = &g_recipe.files[media->first_file + i];
    recipe_file->present = file_exists(recipe_file->target);
    if (!recipe_file->present) has_missing_target = true;
  }

  char image[PATH_CAP] = "";
  iso9660_layout_t layout = {0, 2048, 0};
  const bool is_iso = ends_with_ci(archive_name, ".iso");
  if (has_missing_target && !is_iso &&
      !stage_media(media, archive, archive_name, image, sizeof(image), &layout)) {
    text_copy(g_status, sizeof(g_status), "Could not stage an ISO from this ZIP. It may be incomplete or unsupported.");
    report(g_status, 0, g_total, COL_ERROR);
    return false;
  }
  const char *image_to_read = is_iso ? archive : image;
  bool found_source = false;
  bool fatal_error = false;
  for (int i = 0; i < media->file_count; ++i) {
    recipe_file_t *recipe_file = &g_recipe.files[media->first_file + i];
    if (recipe_file->present || file_exists(recipe_file->target)) {
      recipe_file->present = true;
      g_done++;
      char skipped[STATUS_CAP] = "Already present; skipped ";
      (void)text_append(skipped, sizeof(skipped), recipe_file->target);
      report(skipped, g_done, g_total, COL_OK);
      continue;
    }

    char status[STATUS_CAP] = "Extracting ";
    (void)text_append(status, sizeof(status), recipe_file->source);
    (void)text_append(status, sizeof(status), " -> ");
    (void)text_append(status, sizeof(status), recipe_file->target);
    g_byte_progress_active = true;
    g_byte_done = 0;
    g_byte_total = 0;
    g_byte_rate_bps = 0;
    g_last_progress_ui_us = 0;
    g_last_progress_log_us = 0;
    g_progress_sample_us = 0;
    g_progress_sample_bytes = 0;
    report(status, g_done, g_total, COL_TEXT);
    const int result = extract_one(image_to_read, recipe_file->source,
                                   recipe_file->target, &layout);
    g_byte_progress_active = false;
    if (result == -103) {
      char skipped[STATUS_CAP] = "Not on this disc; kept missing: ";
      (void)text_append(skipped, sizeof(skipped), recipe_file->source);
      report(skipped, ++g_done, g_total, COL_WARN);
      continue;
    }
    if (result == 1) {
      recipe_file->present = true;
      found_source = true;
      report("Already present; kept the existing output.", ++g_done, g_total, COL_OK);
      continue;
    }
    if (result < 0) {
      char failure[STATUS_CAP] = "Extraction failed for ";
      (void)text_append(failure, sizeof(failure), recipe_file->source);
      const char *reason = result <= -101 && result >= -107 ?
          (result == -102 ? "ISO volume descriptor not found" :
           result == -104 ? "ISO entry is empty" :
           result == -105 ? "file extent is outside the ISO" :
           result == -107 ? "file exceeds the 512 MiB safety limit" :
                            "ISO read or output write failed") :
          (result == -1 ? "could not create the target folder" :
           result == -2 ? "temporary output path is too long" :
           result == -3 ? "could not open the disc image" :
           result == -4 ? "could not create the temporary output file" :
           result == -5 ? "could not finish writing the output file" :
           result == -6 ? "could not commit the completed output file" : NULL);
      if (reason != NULL) {
        (void)text_append(failure, sizeof(failure), " (");
        (void)text_append(failure, sizeof(failure), reason);
        (void)text_append(failure, sizeof(failure), ")");
      }
      text_copy(g_status, sizeof(g_status), failure);
      report(g_status, g_done, g_total, COL_ERROR);
      fatal_error = true;
      break;
    }
    found_source = true;
    recipe_file->present = true;
    g_done++;
    status[0] = '\0';
    (void)text_append(status, sizeof(status), "Extracted ");
    (void)text_append(status, sizeof(status), recipe_file->source);
    (void)text_append(status, sizeof(status), " -> ");
    (void)text_append(status, sizeof(status), recipe_file->target);
    report(status, g_done, g_total, COL_OK);
  }

  g_success = required_files_ready();
  if (g_success) {
    text_copy(g_status, sizeof(g_status), "Required files are ready. Existing files were kept; optional discs can add more later.");
    if (!fatal_error) remove_staged_cache_files();
    report(g_status, g_done, g_total, COL_OK);
  } else if (!fatal_error) {
    text_copy(g_status, sizeof(g_status), found_source ?
        "Disc checked. Required files are still missing; choose another disc to continue." :
        "No missing recipe files were found on this disc. Choose another disc to continue.");
    report(g_status, g_done, g_total, COL_WARN);
  }
  return !fatal_error;
}

static int teardown_ui_on_host(void *unused)
{
  (void)unused;
  if (g_touch_indev != NULL) {
    lv_indev_delete(g_touch_indev);
    g_touch_indev = NULL;
  }
  if (g_display != NULL) {
    lv_display_delete(g_display);
    g_display = NULL;
  }
  return 0;
}

__attribute__((section(".text.entry")))
int app_entry(const app_services_t *svc)
{
  if (svc == NULL || svc->abi_version != PAX_ABI_VERSION || svc->ui_call_sync == NULL ||
      svc->display_write_frame_custom == NULL || svc->mem_caps_alloc == NULL ||
      svc->mem_free == NULL || svc->app_get_arg == NULL || svc->get_time_us == NULL ||
      svc->file_open == NULL || svc->file_read == NULL || svc->file_close == NULL)
    return -1;
  g_svc = svc;
  char json_path[PATH_CAP] = "";
  if (svc->app_get_arg(json_path, sizeof(json_path)) <= 0 || !path_under_roms(json_path)) {
    text_copy(g_status, sizeof(g_status), "No valid local app-info JSON was passed to the installer.");
    return -2;
  }
  pax_file_stat_t info = {0};
  if (svc->file_stat == NULL || svc->file_stat(json_path, &info) != 0 || info.is_dir ||
      info.size == 0 || info.size > MAX_JSON_BYTES) {
    text_copy(g_status, sizeof(g_status), "Recipe JSON is missing or too large.");
    return -3;
  }
  char *json = svc->mem_alloc((size_t)info.size + 1);
  if (json == NULL) return -4;
  void *file = svc->file_open(json_path, "rb");
  if (file == NULL) { svc->mem_free(json); return -5; }
  const size_t length = svc->file_read(json, 1, (size_t)info.size, file);
  (void)svc->file_close(file);
  json[length] = '\0';
  const bool parsed = length == info.size && parse_recipe(json, (int)length) &&
                      ensure_redalert_base_mix(json_path);
  svc->mem_free(json);
  if (!parsed) {
    text_copy(g_status, sizeof(g_status), "This JSON has no supported extraction recipe.");
    return -6;
  }

  if (svc->display_get_size != NULL) {
    int offered_w = 0, offered_h = 0;
    svc->display_get_size(&offered_w, &offered_h);
    if (offered_w >= 320 && offered_h >= 240) { g_width = offered_w; g_height = offered_h; }
  }
  (void)scan_incoming_files();
  g_focused_incoming = 0;
  g_requested_incoming = -1;
  g_exit_requested = false;
  g_processing_media = false;
  g_touch_was_down = false;
  if (g_download_rows > 0) {
    text_copy(g_status, sizeof(g_status), "Download missing game files directly, or select a local disc image below.");
  } else if (g_incoming_count > 0) {
    char initial_status[STATUS_CAP] = "Found ";
    (void)append_u32(initial_status, sizeof(initial_status), (uint32_t)g_incoming_count);
    (void)text_append(initial_status, sizeof(initial_status), " disc image(s). Select one to continue extraction.");
    text_copy(g_status, sizeof(g_status), initial_status);
  } else {
    text_copy(g_status, sizeof(g_status), "No disc images found. Copy .ZIP or .ISO files into the incoming folder.");
  }
  g_canvas = svc->mem_caps_alloc((size_t)g_width * (size_t)g_height * sizeof(uint16_t),
                                PAX_MEM_CAP_SPIRAM | PAX_MEM_CAP_DMA);
  if (g_canvas == NULL) return -7;
  memset(g_canvas, 0, (size_t)g_width * (size_t)g_height * sizeof(uint16_t));
  if (svc->ui_call_sync(build_ui_on_host, NULL, 10000) != 0) {
    svc->mem_free(g_canvas); g_canvas = NULL; return -8;
  }
  /* Do not treat the A press which opened this PAX as a selection in its
   * own disc list. Wait until launch input is released before accepting UI
   * navigation. */
  for (;;) {
    pax_gamepad_state_t pad = {0};
    if (svc->input_gamepad_read != NULL) svc->input_gamepad_read(&pad);
    if (!pad.values[PAX_INPUT_A] && !pad.values[PAX_INPUT_B] &&
        !pad.values[PAX_INPUT_MENU] &&
        (svc->input_l3_read == NULL || !svc->input_l3_read())) break;
    svc->delay_ms(20);
  }
  int previous[PAX_INPUT_MAX] = {0};
  for (;;) {
    pax_gamepad_state_t pad = {0};
    if (svc->input_gamepad_read != NULL) svc->input_gamepad_read(&pad);
    const bool up = pad.values[PAX_INPUT_UP] != 0;
    const bool down = pad.values[PAX_INPUT_DOWN] != 0;
    const bool a = pad.values[PAX_INPUT_A] != 0;
    const bool b = pad.values[PAX_INPUT_B] != 0;
    const bool menu = pad.values[PAX_INPUT_MENU] != 0;
    if (up && !previous[PAX_INPUT_UP] && svc->log_printf != NULL)
      svc->log_printf("recipe: gamepad UP edge (focused=%d items=%d)\n",
                      g_focused_incoming, g_incoming_count);
    if (down && !previous[PAX_INPUT_DOWN] && svc->log_printf != NULL)
      svc->log_printf("recipe: gamepad DOWN edge (focused=%d items=%d)\n",
                      g_focused_incoming, g_incoming_count);
    if (g_instructions_overlay != NULL &&
        ((up && !previous[PAX_INPUT_UP]) || (down && !previous[PAX_INPUT_DOWN]))) {
      (void)svc->ui_call_sync(scroll_instructions_on_host, (void *)(intptr_t)(up ? 100 : -100), 1000);
    } else if (up && !previous[PAX_INPUT_UP] && g_incoming_count > 0) {
      const int old = g_focused_incoming;
      const int next = g_focused_incoming > 0 ? g_focused_incoming - 1 : g_incoming_count - 1;
      const int result = svc->ui_call_sync(focus_incoming_on_host, (void *)(intptr_t)next, 1000);
      if (svc->log_printf != NULL)
        svc->log_printf("recipe: gamepad UP focus=%d->%d ui_result=%d\n",
                        old, next, result);
    } else if (down && !previous[PAX_INPUT_DOWN] && g_incoming_count > 0) {
      const int old = g_focused_incoming;
      const int next = g_focused_incoming + 1 < g_incoming_count ? g_focused_incoming + 1 : 0;
      const int result = svc->ui_call_sync(focus_incoming_on_host, (void *)(intptr_t)next, 1000);
      if (svc->log_printf != NULL)
        svc->log_printf("recipe: gamepad DOWN focus=%d->%d ui_result=%d\n",
                        old, next, result);
    }
    if (g_instructions_overlay == NULL && a && !previous[PAX_INPUT_A] && g_incoming_count > 0) {
      if (svc->log_printf != NULL)
        svc->log_printf("recipe: gamepad A select focus=%d\n", g_focused_incoming);
      g_requested_incoming = g_focused_incoming;
    }
    if ((b && !previous[PAX_INPUT_B]) || (menu && !previous[PAX_INPUT_MENU]) ||
        (svc->input_l3_read != NULL && svc->input_l3_read())) {
      if (svc->log_printf != NULL)
        svc->log_printf("recipe: gamepad Back pressed (B=%d menu=%d)\n", b, menu);
      g_exit_requested = true;
    }
    memcpy(previous, pad.values, sizeof(previous));

    const int requested = g_requested_incoming;
    if (requested >= 0) {
      g_requested_incoming = -1;
      g_processing_media = true;
      (void)svc->ui_call_sync(set_processing_ui_on_host, NULL, 1500);
      (void)run_recipe_for_incoming(requested);
      g_processing_media = false;
      (void)svc->ui_call_sync(set_processing_ui_on_host, NULL, 1500);
    }
    if (g_exit_requested) break;
    if (svc->ui_call_sync(pump_ui_on_host, NULL, 1000) != 0 && svc->log_printf != NULL)
      svc->log_printf("recipe: UI event pump failed\n");
    svc->delay_ms(20);
  }
  (void)svc->ui_call_sync(teardown_ui_on_host, NULL, 2000);
  svc->mem_free(g_canvas); g_canvas = NULL;
  return 0;
}
