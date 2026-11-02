/* Host regression tests for the real NES page provider and legacy renderer.
 * Build with the cached nofrendo headers, not a second implementation. */
#define PAX_APP_SIDE 1
#include "../pax_nes_options.c"
#include <assert.h>
#include <stdlib.h>
#include <sys/stat.h>

const app_services_t *_pax_svc;
volatile int pax_exit_requested;
rgb_t nes_palette[64], shady_palette[64];
nes_t *nes_getcontextptr(void) { return NULL; }
void ppu_setpal(ppu_t *ppu, rgb_t *pal) { (void)ppu; (void)pal; }
static int resets, saves, loads, closes, returns_to_base;
void nes_reset(int type) { (void)type; ++resets; }
void state_setslot(int slot) { assert(slot >= 0 && slot < 10); }

static char test_root[512];
static void host_path(const char *path, char *out) {
    const char *name = strrchr(path, '/');
    snprintf(out, 768, "%s/%s", test_root, name ? name + 1 : path);
}
static void *host_open(const char *path, const char *mode) {
    char p[768]; host_path(path, p); return fopen(p, mode);
}
static int host_close(void *f) { return fclose(f); }
static size_t host_read(void *p, size_t s, size_t n, void *f) { return fread(p, s, n, f); }
static size_t host_write(const void *p, size_t s, size_t n, void *f) { return fwrite(p, s, n, f); }
static int host_remove(const char *path) { char p[768]; host_path(path, p); return remove(p); }
static int host_rename(const char *from, const char *to) {
    char a[768], b[768]; host_path(from, a); host_path(to, b); return rename(a, b);
}
static int host_stat(const char *path, pax_file_stat_t *out) {
    char p[768]; struct stat st; host_path(path, p);
    if (stat(p, &st) != 0) return -1;
    memset(out, 0, sizeof(*out)); out->is_dir = S_ISDIR(st.st_mode); return 0;
}
static int host_mkdir(const char *path) { (void)path; return 0; }
static void *host_alloc(size_t n, uint32_t caps) { (void)caps; return malloc(n); }
static char *host_rom(void) { return "/sd/roms/nes/regression.nes"; }
int pax_nes_state_save(const char *path) {
    void *f = host_open(path, "wb"); assert(f);
    assert(host_write("TESTSNSS", 1, 8, f) == 8); assert(host_close(f) == 0); ++saves; return 0;
}
int pax_nes_state_load(const char *path) {
    char data[8]; void *f = host_open(path, "rb"); assert(f);
    assert(host_read(data, 1, 8, f) == 8); assert(memcmp(data, "TESTSNSS", 8) == 0);
    assert(host_close(f) == 0); ++loads; return 0;
}

static pax_game_menu_action_fn_t action_callback;
static void *action_context;
static pax_game_menu_page_t displayed;
static uint16_t preview_copy[NES_THUMB_W * NES_THUMB_H];
static int host_register(const char *label, pax_game_menu_action_fn_t callback, void *context) {
    if (callback) assert(strcmp(label, "NES OPTIONS") == 0);
    action_callback = callback; action_context = context; return 0;
}
static int host_set_page(const pax_game_menu_page_t *page) {
    if (!page) { ++returns_to_base; return 0; }
    assert(page->count > 0 && page->count <= PAX_GAME_MENU_MAX_ITEMS);
    assert(page->selected < page->count);
    displayed = *page;
    if (page->preview) {
        memcpy(preview_copy, page->preview, sizeof(preview_copy)); displayed.preview = preview_copy;
    }
    return 0;
}
static void host_menu_close(void) { ++closes; }
static void choose(int row, int direction) {
    assert(action_callback); action_callback(action_context, displayed.items[row].action, direction);
}
static int submitted;
static uint16_t submitted_frame[NES_MENU_W * NES_MENU_H];
static void host_display(const uint16_t *frame, uint16_t w, uint16_t h, float scale, bool turn) {
    (void)scale; (void)turn; assert(w == NES_MENU_W && h == NES_MENU_H);
    memcpy(submitted_frame, frame, sizeof(submitted_frame)); ++submitted;
}

int main(int argc, char **argv) {
    assert(argc == 2); snprintf(test_root, sizeof(test_root), "%s", argv[1]);
    app_services_t svc = {0};
    svc.settings_rom_path_get = host_rom;
    svc.file_open = host_open; svc.file_close = host_close;
    svc.file_read = host_read; svc.file_write = host_write; svc.file_stat = host_stat;
    svc.file_remove = host_remove; svc.file_rename = host_rename; svc.file_mkdir = host_mkdir;
    svc.mem_caps_alloc = host_alloc; svc.mem_free = free;
    svc.game_menu_register = host_register; svc.game_menu_set_page = host_set_page;
    svc.game_menu_close = host_menu_close; svc.display_write_frame_custom = host_display;
    static uint16_t frame[NES_MENU_W * NES_MENU_H], modal[NES_MENU_W * NES_MENU_H];
    for (size_t i = 0; i < sizeof(frame) / sizeof(frame[0]); ++i) frame[i] = (uint16_t)(i + 1);
    assert(pax_nes_shared_menu_attach(&svc, frame, NES_MENU_W, NES_MENU_H));
    action_callback(action_context, PAX_GAME_MENU_ACTION_OPEN, 0);
    assert(displayed.count == 8 && strstr(displayed.items[0].label, "SLOT 0: EMPTY"));
    choose(2, 0); assert(!loads && !closes && !strcmp(displayed.status, "EMPTY SLOT"));
    choose(0, -1); assert(strstr(displayed.items[0].label, "SLOT 9: EMPTY"));
    choose(0, 1); assert(strstr(displayed.items[0].label, "SLOT 0: EMPTY"));
    choose(1, 0); assert(saves == 1 && !strcmp(displayed.status, "STATE SAVED"));
    assert(displayed.preview && preview_copy[0] == frame[0]);
    choose(1, 0); assert(saves == 1 && !strcmp(displayed.status, "A AGAIN TO OVERWRITE"));
    choose(1, 0); assert(saves == 2);
    choose(2, 0); assert(loads == 1 && closes == 1);
    action_callback(action_context, PAX_GAME_MENU_ACTION_OPEN, 0);
    choose(3, 0); assert(!strcmp(displayed.status, "A AGAIN TO DELETE"));
    choose(0, 1); choose(0, -1); choose(3, 0);
    assert(nes_shared_slot_exists() && !strcmp(displayed.status, "A AGAIN TO DELETE"));
    choose(3, 0); assert(!nes_shared_slot_exists() && !strcmp(displayed.status, "STATE DELETED"));
    choose(6, 0); assert(displayed.count == 4 && !strcmp(displayed.title, "NES VIDEO OPTIONS"));
    float initial_scale = pax_nes_options_scale(); choose(2, -1);
    assert(pax_nes_options_scale() != initial_scale && !strcmp(displayed.status, "OPTION SAVED"));
    pax_nes_options_load_for_current_rom(&svc); assert(pax_nes_options_scale() != initial_scale);
    action_callback(action_context, displayed.back_action, 0); assert(displayed.count == 8);
    action_callback(action_context, displayed.back_action, 0); assert(returns_to_base == 1 && closes == 1);
    action_callback(action_context, PAX_GAME_MENU_ACTION_OPEN, 0); choose(5, 0);
    assert(resets == 1 && closes == 2);
    pax_nes_shared_menu_detach(); assert(!action_callback);
    svc.game_menu_register = NULL;
    assert(!pax_nes_shared_menu_attach(&svc, frame, NES_MENU_W, NES_MENU_H));
    nes_draw_menu(&svc, frame, modal, NES_MENU_W, NES_MENU_H, preview_copy,
                  nes_rom_hash(&svc), NES_PAGE_ROOT, 0, 0, -1, NULL);
    assert(submitted == 1 && memcmp(submitted_frame, modal, sizeof(modal)) == 0);
    assert(memcmp(submitted_frame, frame, sizeof(frame)) != 0);
    puts("PASS: in-place pages, 10 slots, save/load/confirm/delete, previews, video persistence, Back, legacy pixels");
    return 0;
}
