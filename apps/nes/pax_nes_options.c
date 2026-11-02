#include "pax_nes_options.h"

#include <nes.h>
#include <nes_pal.h>
#include <nes_ppu.h>
#include <nesstate.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern const app_services_t *_pax_svc;
extern volatile int pax_exit_requested;

#define NES_MENU_W 256
#define NES_MENU_H 224
#define NES_STATE_SLOTS 10
#define NES_THUMB_W 64
#define NES_THUMB_H 56
#define NES_THUMB_HEADER_SIZE 8
#define NES_SLOT_DIR "/sd/roms/nes/.states"
#define NES_MENU_VISIBLE_SLOTS 6

#define NES_COLOR_BLACK  0x0000
#define NES_COLOR_WHITE  0xFFFF
#define NES_COLOR_YELLOW 0xFFE0
#define NES_COLOR_GREEN  0x07E0
#define NES_COLOR_RED    0xF800
#define NES_COLOR_GRAY   0x8410

enum {
    NES_MENU_RESUME,
    NES_MENU_SAVE,
    NES_MENU_LOAD,
    NES_MENU_DELETE,
    NES_MENU_SCREENSHOT,
    NES_MENU_RESTART,
    NES_MENU_OPTIONS,
    NES_MENU_EXIT,
    NES_MENU_ROOT_COUNT
};

enum {
    NES_PAGE_ROOT,
    NES_PAGE_SAVE,
    NES_PAGE_LOAD,
    NES_PAGE_DELETE,
    NES_PAGE_OPTIONS
};

enum {
    NES_OPTION_PALETTE,
    NES_OPTION_CROP_SIDES,
    NES_OPTION_ZOOM,
    NES_OPTION_COUNT
};

typedef struct {
    int palette;
    int crop_sides;
    int zoom;
} nes_options_t;

static nes_options_t nes_options = {0, 0, 1};
static int nes_palette_pending = 1;
static uint32_t nes_loaded_rom_hash;
static int nes_touch_active;
static int nes_touch_swipe_fired;
static int nes_touch_start_y;
static const float nes_zoom_values[] = {1.5f, 2.0f, 2.5f};
static const char *const nes_option_labels[NES_OPTION_COUNT] = {
    "PALETTE", "CROP SIDES", "ZOOM"
};

int pax_nes_options_crop_sides(void)
{
    return nes_options.crop_sides;
}

float pax_nes_options_scale(void)
{
    int zoom = nes_options.zoom;
    if (zoom < 0 || zoom >= (int)(sizeof(nes_zoom_values) / sizeof(nes_zoom_values[0])))
        zoom = 1;
    return nes_zoom_values[zoom];
}

/* Compact 5x7 font: space, A-Z, 0-9, >, :, -, ., *. */
static const uint8_t nes_font[][7] = {
    {0,0,0,0,0,0,0},
    {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},
    {30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
    {14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},
    {7,2,2,2,2,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},
    {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},
    {17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4},{31,1,2,4,8,16,31},
    {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},
    {30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
    {6,8,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},
    {14,17,17,15,1,2,12},
    {16,8,4,2,4,8,16},{0,4,0,0,0,4,0},{0,0,0,31,0,0,0},
    {0,0,0,0,0,0,4},{0,0,14,1,15,17,15}
};

static const char *const root_labels[NES_MENU_ROOT_COUNT] = {
    "RESUME GAME", "SAVE STATE", "LOAD STATE", "DELETE STATE",
    "SCREENSHOT", "RESTART GAME", "OPTIONS", "EXIT TO LAUNCHER"
};

static int nes_font_index(char ch)
{
    if (ch == ' ')
        return 0;
    if (ch >= 'a' && ch <= 'z')
        ch = (char)(ch - 'a' + 'A');
    if (ch >= 'A' && ch <= 'Z')
        return 1 + ch - 'A';
    if (ch >= '0' && ch <= '9')
        return 27 + ch - '0';
    switch (ch) {
        case '>': return 37;
        case ':': return 38;
        case '-': return 39;
        case '.': return 40;
        case '*': return 41;
        default: return 0;
    }
}

static void nes_fill(uint16_t *fb, int x, int y, int w, int h, uint16_t color)
{
    if (!fb || w <= 0 || h <= 0)
        return;
    for (int py = y; py < y + h; ++py) {
        if (py < 0 || py >= NES_MENU_H)
            continue;
        for (int px = x; px < x + w; ++px) {
            if (px >= 0 && px < NES_MENU_W)
                fb[py * NES_MENU_W + px] = color;
        }
    }
}

static void nes_draw_char(uint16_t *fb, int x, int y, char ch, uint16_t color)
{
    const uint8_t *glyph = nes_font[nes_font_index(ch)];
    for (int row = 0; row < 7; ++row) {
        for (int col = 0; col < 5; ++col) {
            if (glyph[row] & (0x10u >> col))
                nes_fill(fb, x + col, y + row, 1, 1, color);
        }
    }
}

static void nes_draw_text(uint16_t *fb, int x, int y, const char *text, uint16_t color)
{
    while (text && *text) {
        nes_draw_char(fb, x, y, *text++, color);
        x += 6;
    }
}

static void nes_draw_centered(uint16_t *fb, int y, const char *text, uint16_t color)
{
    const int width = text ? (int)strlen(text) * 6 : 0;
    nes_draw_text(fb, (NES_MENU_W - width) / 2, y, text, color);
}

static void nes_draw_frame(const app_services_t *svc, const uint16_t *game_frame,
                           uint16_t *menu_frame, uint16_t width, uint16_t height)
{
    const size_t bytes = (size_t)NES_MENU_W * NES_MENU_H * sizeof(uint16_t);
    if (game_frame && width == NES_MENU_W && height == NES_MENU_H)
        memcpy(menu_frame, game_frame, bytes);
    else
        memset(menu_frame, 0, bytes);

}

static bool nes_path_exists(const app_services_t *svc, const char *path)
{
    pax_file_stat_t st;
    return svc->file_stat && svc->file_stat(path, &st) == 0 && !st.is_dir;
}

static void nes_remove_if_present(const app_services_t *svc, const char *path)
{
    if (svc->file_remove && nes_path_exists(svc, path))
        (void)svc->file_remove(path);
}

static uint32_t nes_rom_hash(const app_services_t *svc)
{
    char identity[512] = {0};
    const char *configured = svc->settings_rom_path_get ? svc->settings_rom_path_get() : NULL;
    if (configured && configured[0]) {
        strncpy(identity, configured, sizeof(identity) - 1);
    } else if (svc->file_open && svc->file_read && svc->file_close) {
        void *file = svc->file_open("/sd/roms/pax/nes.rom", "rb");
        if (file) {
            size_t n = svc->file_read(identity, 1, sizeof(identity) - 1, file);
            svc->file_close(file);
            identity[n] = '\0';
            identity[strcspn(identity, "\r\n")] = '\0';
        }
    }
    if (!identity[0])
        strcpy(identity, "/sd/roms/nes/River City Ransom (USA).nes");

    uint32_t hash = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)identity; *p; ++p)
        hash = (hash ^ *p) * 16777619u;
    return hash;
}

static int nes_state_paths(uint32_t rom_hash, int slot, char *state,
                           size_t state_size, char *preview, size_t preview_size,
                           char *temp, size_t temp_size, char *backup, size_t backup_size)
{
    if (slot < 0 || slot >= NES_STATE_SLOTS)
        return -1;
    int n1 = snprintf(state, state_size, NES_SLOT_DIR "/%08lx_%02d.snss",
                      (unsigned long)rom_hash, slot);
    int n2 = snprintf(preview, preview_size, NES_SLOT_DIR "/%08lx_%02d.thumb",
                      (unsigned long)rom_hash, slot);
    int n3 = snprintf(temp, temp_size, NES_SLOT_DIR "/%08lx_%02d.tmp",
                      (unsigned long)rom_hash, slot);
    int n4 = snprintf(backup, backup_size, NES_SLOT_DIR "/%08lx_%02d.bak",
                      (unsigned long)rom_hash, slot);
    return n1 > 0 && (size_t)n1 < state_size && n2 > 0 && (size_t)n2 < preview_size &&
           n3 > 0 && (size_t)n3 < temp_size && n4 > 0 && (size_t)n4 < backup_size ? 0 : -1;
}

static int nes_replace_file(const app_services_t *svc, const char *temp,
                            const char *final, const char *backup)
{
    nes_remove_if_present(svc, backup);
    const bool had_final = nes_path_exists(svc, final);
    if (had_final && svc->file_rename(final, backup) != 0)
        return -1;
    if (svc->file_rename(temp, final) != 0) {
        if (had_final)
            (void)svc->file_rename(backup, final);
        return -1;
    }
    if (had_final)
        (void)svc->file_remove(backup);
    return 0;
}

static int nes_config_paths(uint32_t rom_hash, char *path, size_t path_size,
                            char *temp, size_t temp_size,
                            char *backup, size_t backup_size)
{
    const int n1 = snprintf(path, path_size, NES_SLOT_DIR "/%08lx.cfg",
                            (unsigned long)rom_hash);
    const int n2 = snprintf(temp, temp_size, NES_SLOT_DIR "/%08lx.cfg.tmp",
                            (unsigned long)rom_hash);
    const int n3 = snprintf(backup, backup_size, NES_SLOT_DIR "/%08lx.cfg.bak",
                            (unsigned long)rom_hash);
    return n1 > 0 && (size_t)n1 < path_size && n2 > 0 && (size_t)n2 < temp_size &&
           n3 > 0 && (size_t)n3 < backup_size ? 0 : -1;
}

static void nes_apply_palette(void)
{
    nes_t *machine = nes_getcontextptr();
    if (machine && machine->ppu) {
        ppu_setpal(machine->ppu, nes_options.palette ? shady_palette : nes_palette);
        nes_palette_pending = 0;
    }
}

void pax_nes_options_apply_palette(void)
{
    if (nes_palette_pending)
        nes_apply_palette();
}

void pax_nes_options_load(const app_services_t *svc, uint32_t rom_hash)
{
    nes_options = (nes_options_t){0, 0, 1};
    nes_loaded_rom_hash = rom_hash;
    nes_palette_pending = 1;
    char path[96], temp[96], backup[96];
    if (svc && svc->file_open && svc->file_read && svc->file_close &&
        nes_config_paths(rom_hash, path, sizeof(path), temp, sizeof(temp),
                         backup, sizeof(backup)) == 0) {
        void *file = svc->file_open(path, "rb");
        if (file) {
            char data[64] = {0};
            const size_t n = svc->file_read(data, 1, sizeof(data) - 1, file);
            svc->file_close(file);
            data[n] = '\0';
            char magic[8] = {0};
            int palette = 0, crop_sides = 0, zoom = 1;
            if (sscanf(data, "%7s %d %d %d", magic, &palette, &crop_sides, &zoom) == 4 &&
                strcmp(magic, "NESOPT1") == 0 && palette >= 0 && palette <= 1 &&
                crop_sides >= 0 && crop_sides <= 1 && zoom >= 0 && zoom <= 2) {
                nes_options.palette = palette;
                nes_options.crop_sides = crop_sides;
                nes_options.zoom = zoom;
            }
        }
    }
    nes_apply_palette();
}

void pax_nes_options_load_for_current_rom(const app_services_t *svc)
{
    pax_nes_options_load(svc, nes_rom_hash(svc));
}

static int nes_save_options(const app_services_t *svc, uint32_t rom_hash)
{
    if (!svc || !svc->file_mkdir || !svc->file_open || !svc->file_write ||
        !svc->file_close || !svc->file_rename || !svc->file_remove ||
        !svc->file_stat || svc->file_mkdir(NES_SLOT_DIR) != 0)
        return -1;

    char path[96], temp[96], backup[96], data[48];
    if (nes_config_paths(rom_hash, path, sizeof(path), temp, sizeof(temp),
                         backup, sizeof(backup)) != 0)
        return -1;
    const int len = snprintf(data, sizeof(data), "NESOPT1 %d %d %d\n",
                             nes_options.palette, nes_options.crop_sides, nes_options.zoom);
    if (len <= 0 || (size_t)len >= sizeof(data))
        return -1;

    nes_remove_if_present(svc, temp);
    nes_remove_if_present(svc, backup);
    void *file = svc->file_open(temp, "wb");
    if (!file)
        return -1;
    const int written = svc->file_write(data, 1, (size_t)len, file) == (size_t)len;
    const int closed = svc->file_close(file) == 0;
    if (!written || !closed) {
        nes_remove_if_present(svc, temp);
        return -1;
    }
    return nes_replace_file(svc, temp, path, backup);
}

static const char *nes_option_value(int option)
{
    switch (option) {
        case NES_OPTION_PALETTE:
            return nes_options.palette ? "SHADY" : "NOFRENDO";
        case NES_OPTION_CROP_SIDES:
            return nes_options.crop_sides ? "8 PX" : "OFF";
        case NES_OPTION_ZOOM:
            return nes_options.zoom == 0 ? "1.5X" :
                   nes_options.zoom == 2 ? "2.5X" : "2.0X";
        default:
            return "?";
    }
}

static int nes_change_option(const app_services_t *svc, uint32_t rom_hash,
                             int option, int direction)
{
    switch (option) {
        case NES_OPTION_PALETTE:
            nes_options.palette = !nes_options.palette;
            nes_apply_palette();
            break;
        case NES_OPTION_CROP_SIDES:
            nes_options.crop_sides = !nes_options.crop_sides;
            break;
        case NES_OPTION_ZOOM:
            nes_options.zoom = (nes_options.zoom + direction + 3) % 3;
            break;
        default:
            return -1;
    }
    return nes_save_options(svc, rom_hash);
}

static int nes_write_preview(const app_services_t *svc, const char *path,
                             const char *backup, const uint16_t *frame)
{
    if (!frame || !svc->mem_caps_alloc || !svc->mem_free || !svc->file_open ||
        !svc->file_write || !svc->file_close || !svc->file_rename || !svc->file_remove)
        return -1;
    uint16_t *thumb = (uint16_t *)svc->mem_caps_alloc(
        NES_THUMB_W * NES_THUMB_H * sizeof(uint16_t), PAX_MEM_CAP_SPIRAM);
    if (!thumb)
        return -1;
    for (int y = 0; y < NES_THUMB_H; ++y)
        for (int x = 0; x < NES_THUMB_W; ++x)
            thumb[y * NES_THUMB_W + x] = frame[(y * 4) * NES_MENU_W + x * 4];

    char temp[128];
    int len = snprintf(temp, sizeof(temp), "%s.tmp", path);
    if (len <= 0 || (size_t)len >= sizeof(temp)) {
        svc->mem_free(thumb);
        return -1;
    }
    nes_remove_if_present(svc, temp);
    void *file = svc->file_open(temp, "wb");
    if (!file) {
        svc->mem_free(thumb);
        return -1;
    }
    const uint8_t header[NES_THUMB_HEADER_SIZE] = {'N','T','H','1', NES_THUMB_W,0, NES_THUMB_H,0};
    int ok = svc->file_write(header, 1, sizeof(header), file) == sizeof(header) &&
             svc->file_write(thumb, 1, NES_THUMB_W * NES_THUMB_H * sizeof(uint16_t), file) ==
                 NES_THUMB_W * NES_THUMB_H * sizeof(uint16_t);
    if (svc->file_close(file) != 0)
        ok = 0;
    svc->mem_free(thumb);
    if (!ok) {
        nes_remove_if_present(svc, temp);
        return -1;
    }
    return nes_replace_file(svc, temp, path, backup);
}

static int nes_read_preview(const app_services_t *svc, uint32_t rom_hash, int slot,
                            uint16_t *thumb)
{
    char state[128], path[128], temp[128], backup[128];
    if (!svc->file_open || !svc->file_read || !svc->file_close ||
        nes_state_paths(rom_hash, slot, state, sizeof(state), path, sizeof(path),
                        temp, sizeof(temp), backup, sizeof(backup)) != 0)
        return -1;
    void *file = svc->file_open(path, "rb");
    if (!file)
        return -1;
    uint8_t header[NES_THUMB_HEADER_SIZE];
    size_t pixels = NES_THUMB_W * NES_THUMB_H * sizeof(uint16_t);
    int ok = svc->file_read(header, 1, sizeof(header), file) == sizeof(header) &&
             memcmp(header, "NTH1", 4) == 0 && header[4] == NES_THUMB_W && header[5] == 0 &&
             header[6] == NES_THUMB_H && header[7] == 0 &&
             svc->file_read(thumb, 1, pixels, file) == pixels;
    svc->file_close(file);
    return ok ? 0 : -1;
}

static int nes_save_slot(const app_services_t *svc, uint32_t rom_hash, int slot,
                         const uint16_t *game_frame)
{
    if (!svc->file_mkdir || !svc->file_rename || !svc->file_remove || !svc->file_stat ||
        svc->file_mkdir(NES_SLOT_DIR) != 0)
        return -1;

    char state[128], preview[128], temp[128], backup[128];
    if (nes_state_paths(rom_hash, slot, state, sizeof(state), preview, sizeof(preview),
                        temp, sizeof(temp), backup, sizeof(backup)) != 0)
        return -1;
    nes_remove_if_present(svc, temp);
    nes_remove_if_present(svc, backup);
    state_setslot(slot);
    if (pax_nes_state_save(temp) != 0) {
        nes_remove_if_present(svc, temp);
        return -1;
    }
    if (nes_replace_file(svc, temp, state, backup) != 0) {
        nes_remove_if_present(svc, temp);
        return -1;
    }

    char thumb_backup[128];
    int n = snprintf(thumb_backup, sizeof(thumb_backup), "%s.bak", preview);
    if (n > 0 && (size_t)n < sizeof(thumb_backup) &&
        nes_write_preview(svc, preview, thumb_backup, game_frame) != 0 && svc->log_printf)
        svc->log_printf("NES PAX: state slot %d saved, but thumbnail could not be written\n", slot);
    return 0;
}

static int nes_load_slot(const app_services_t *svc, uint32_t rom_hash, int slot)
{
    char state[128], preview[128], temp[128], backup[128];
    if (nes_state_paths(rom_hash, slot, state, sizeof(state), preview, sizeof(preview),
                        temp, sizeof(temp), backup, sizeof(backup)) != 0)
        return -1;
    if (!nes_path_exists(svc, state))
        return -2;
    state_setslot(slot);
    return pax_nes_state_load(state);
}

static int nes_delete_slot(const app_services_t *svc, uint32_t rom_hash, int slot)
{
    char state[128], preview[128], temp[128], backup[128];
    if (!svc->file_remove ||
        nes_state_paths(rom_hash, slot, state, sizeof(state), preview, sizeof(preview),
                        temp, sizeof(temp), backup, sizeof(backup)) != 0 ||
        !nes_path_exists(svc, state))
        return -1;
    if (svc->file_remove(state) != 0)
        return -1;
    nes_remove_if_present(svc, preview);
    return 0;
}

static void nes_put_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

static void nes_put_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}

static int nes_save_screenshot(const app_services_t *svc, uint32_t rom_hash,
                               const uint16_t *frame)
{
    if (!frame || !svc || !svc->file_mkdir || !svc->file_open || !svc->file_write ||
        !svc->file_close || !svc->file_stat || svc->file_mkdir(NES_SLOT_DIR) != 0)
        return -1;

    static unsigned screenshot_index;
    char path[112];
    const unsigned long long stamp = (unsigned long long)(
        svc->get_time_us ? svc->get_time_us() : screenshot_index);
    int len = -1;
    bool available = false;
    for (unsigned tries = 0; tries < 100; ++tries) {
        len = snprintf(path, sizeof(path), NES_SLOT_DIR "/%08lx_%llx_%02u.bmp",
                       (unsigned long)rom_hash, stamp, screenshot_index++ % 100u);
        if (len <= 0 || (size_t)len >= sizeof(path))
            return -1;
        pax_file_stat_t st;
        if (svc->file_stat(path, &st) != 0) {
            available = true;
            break;
        }
    }
    if (!available || len <= 0 || (size_t)len >= sizeof(path))
        return -1;

    enum { ROW_BYTES = NES_MENU_W * 3, PIXEL_BYTES = ROW_BYTES * NES_MENU_H };
    uint8_t header[54] = {0};
    header[0] = 'B';
    header[1] = 'M';
    nes_put_le32(header + 2, 54u + PIXEL_BYTES);
    nes_put_le32(header + 10, 54);
    nes_put_le32(header + 14, 40);
    nes_put_le32(header + 18, NES_MENU_W);
    nes_put_le32(header + 22, (uint32_t)(-(int)NES_MENU_H));
    nes_put_le16(header + 26, 1);
    nes_put_le16(header + 28, 24);
    nes_put_le32(header + 34, PIXEL_BYTES);

    void *file = svc->file_open(path, "wb");
    if (!file)
        return -1;
    int ok = svc->file_write(header, 1, sizeof(header), file) == sizeof(header);
    uint8_t row[ROW_BYTES];
    for (int y = 0; ok && y < NES_MENU_H; ++y) {
        for (int x = 0; x < NES_MENU_W; ++x) {
            const uint16_t pixel = frame[y * NES_MENU_W + x];
            row[x * 3] = (uint8_t)(((pixel & 0x1fu) * 255u) / 31u);
            row[x * 3 + 1] = (uint8_t)((((pixel >> 5) & 0x3fu) * 255u) / 63u);
            row[x * 3 + 2] = (uint8_t)((((pixel >> 11) & 0x1fu) * 255u) / 31u);
        }
        ok = svc->file_write(row, 1, sizeof(row), file) == sizeof(row);
    }
    if (svc->file_close(file) != 0)
        ok = 0;
    if (!ok && svc->file_remove)
        (void)svc->file_remove(path);
    return ok ? 0 : -1;
}

static void nes_draw_border(uint16_t *fb, int x, int y, int w, int h, uint16_t color)
{
    nes_fill(fb, x, y, w, 1, color);
    nes_fill(fb, x, y + h - 1, w, 1, color);
    nes_fill(fb, x, y, 1, h, color);
    nes_fill(fb, x + w - 1, y, 1, h, color);
}

static void nes_draw_menu(const app_services_t *svc, const uint16_t *game_frame,
                          uint16_t *menu_frame, uint16_t width, uint16_t height,
                          uint16_t *thumb, uint32_t rom_hash, int page,
                          int selected, int scroll, int confirm_slot,
                          const char *message)
{
    nes_draw_frame(svc, game_frame, menu_frame, width, height);
    nes_fill(menu_frame, 12, 10, 232, 204, NES_COLOR_BLACK);
    nes_draw_border(menu_frame, 12, 10, 232, 204, NES_COLOR_WHITE);

    const char *title = page == NES_PAGE_ROOT ? "NES MENU" :
                        page == NES_PAGE_SAVE ? "SAVE STATE" :
                        page == NES_PAGE_LOAD ? "LOAD STATE" : "DELETE STATE";
    if (page == NES_PAGE_OPTIONS)
        title = "NES OPTIONS";
    nes_draw_centered(menu_frame, 22, title, NES_COLOR_YELLOW);

    if (page == NES_PAGE_ROOT) {
        for (int i = 0; i < NES_MENU_ROOT_COUNT; ++i) {
            int y = 42 + i * 18;
            uint16_t color = i == selected ? NES_COLOR_YELLOW : NES_COLOR_WHITE;
            if (i == selected)
                nes_draw_char(menu_frame, 36, y, '>', NES_COLOR_YELLOW);
            nes_draw_text(menu_frame, 48, y, root_labels[i], color);
        }
        if (message)
            nes_draw_centered(menu_frame, 188, message, NES_COLOR_GREEN);
        else
            nes_draw_centered(menu_frame, 188, "A SELECT B RESUME", NES_COLOR_GRAY);
    } else if (page == NES_PAGE_OPTIONS) {
        for (int option = 0; option < NES_OPTION_COUNT; ++option) {
            const int y = 60 + option * 27;
            const uint16_t color = option == selected ? NES_COLOR_YELLOW : NES_COLOR_WHITE;
            if (option == selected)
                nes_draw_char(menu_frame, 30, y, '>', NES_COLOR_YELLOW);
            nes_draw_text(menu_frame, 42, y, nes_option_labels[option], color);
            nes_draw_text(menu_frame, 158, y, nes_option_value(option), color);
        }
        if (message)
            nes_draw_centered(menu_frame, 166, message, NES_COLOR_GREEN);
        nes_draw_centered(menu_frame, 188, "LEFT RIGHT OR A CHANGE", NES_COLOR_GRAY);
    } else {
        nes_draw_text(menu_frame, 28, 43, "SLOT", NES_COLOR_GRAY);
        nes_draw_text(menu_frame, 165, 43, "PREVIEW", NES_COLOR_GRAY);
        const int first = scroll;
        const int last = first + NES_MENU_VISIBLE_SLOTS < NES_STATE_SLOTS ?
                         first + NES_MENU_VISIBLE_SLOTS : NES_STATE_SLOTS;
        for (int slot = first; slot < last; ++slot) {
            char state[128], preview[128], temp[128], backup[128], label[32];
            const bool exists = nes_state_paths(rom_hash, slot, state, sizeof(state),
                preview, sizeof(preview), temp, sizeof(temp), backup, sizeof(backup)) == 0 &&
                nes_path_exists(svc, state);
            snprintf(label, sizeof(label), "SLOT %d %s", slot,
                     exists ? "SAVED" : "EMPTY");
            const int y = 55 + (slot - first) * 17;
            const uint16_t color = slot == selected ? NES_COLOR_YELLOW : NES_COLOR_WHITE;
            if (slot == selected)
                nes_draw_char(menu_frame, 20, y, '>', NES_COLOR_YELLOW);
            nes_draw_text(menu_frame, 31, y, label, color);
        }

        nes_fill(menu_frame, 160, 55, 72, 64, NES_COLOR_BLACK);
        nes_draw_border(menu_frame, 160, 55, 72, 64, NES_COLOR_GRAY);
        if (selected >= 0 && selected < NES_STATE_SLOTS &&
            nes_read_preview(svc, rom_hash, selected, thumb) == 0) {
            for (int y = 0; y < NES_THUMB_H; ++y)
                memcpy(menu_frame + (62 + y) * NES_MENU_W + 164,
                       thumb + y * NES_THUMB_W, NES_THUMB_W * sizeof(uint16_t));
        } else {
            nes_draw_text(menu_frame, 166, 81, "NO IMAGE", NES_COLOR_GRAY);
        }

        if (confirm_slot == selected && page == NES_PAGE_SAVE)
            nes_draw_centered(menu_frame, 166, "A AGAIN TO OVERWRITE", NES_COLOR_RED);
        else if (confirm_slot == selected && page == NES_PAGE_DELETE)
            nes_draw_centered(menu_frame, 166, "A AGAIN TO DELETE", NES_COLOR_RED);
        else if (message)
            nes_draw_centered(menu_frame, 166, message, NES_COLOR_GREEN);

        const char *hint = page == NES_PAGE_SAVE ? "A SAVE B BACK" :
                           page == NES_PAGE_LOAD ? "A LOAD B BACK" : "A DELETE B BACK";
        nes_draw_centered(menu_frame, 188, hint, NES_COLOR_GRAY);
    }
    /* Submit AFTER drawing. Submitting the background first made the legacy
       modal invisible even though it was successfully pausing the emulator. */
    if (svc->display_write_frame_custom)
        svc->display_write_frame_custom(menu_frame, NES_MENU_W, NES_MENU_H,
                                        pax_nes_options_scale(), false);
}

enum {
    NES_SHARED_SLOT = 1,
    NES_SHARED_SAVE,
    NES_SHARED_LOAD,
    NES_SHARED_DELETE,
    NES_SHARED_SCREENSHOT,
    NES_SHARED_RESTART,
    NES_SHARED_VIDEO,
    NES_SHARED_BACK,
    NES_SHARED_PALETTE,
    NES_SHARED_CROP,
    NES_SHARED_ZOOM,
    NES_SHARED_VIDEO_BACK
};

static struct {
    const app_services_t *svc;
    const uint16_t *frame;
    uint16_t *thumb;
    uint32_t rom_hash;
    int slot;
    int confirm_action;
    int video;
    int registered;
} nes_shared;

static bool nes_shared_slot_exists(void)
{
    char state[128], preview[128], temp[128], backup[128];
    return nes_state_paths(nes_shared.rom_hash, nes_shared.slot, state, sizeof(state),
                           preview, sizeof(preview), temp, sizeof(temp), backup, sizeof(backup)) == 0 &&
           nes_path_exists(nes_shared.svc, state);
}

static void nes_shared_publish(int selected, const char *status)
{
    pax_game_menu_page_t page = {0};
    snprintf(page.title, sizeof(page.title), "%s", nes_shared.video ? "NES VIDEO OPTIONS" : "NES OPTIONS");
    if (status)
        snprintf(page.status, sizeof(page.status), "%s", status);
    page.selected = selected;
    if (nes_shared.video) {
        page.count = 4;
        page.back_action = NES_SHARED_VIDEO_BACK;
        for (int i = 0; i < NES_OPTION_COUNT; ++i) {
            page.items[i].action = NES_SHARED_PALETTE + i;
            page.items[i].flags = PAX_GAME_MENU_ITEM_ADJUSTABLE;
            snprintf(page.items[i].label, sizeof(page.items[i].label), "%s: %s",
                     nes_option_labels[i], nes_option_value(i));
        }
        page.items[3].action = NES_SHARED_VIDEO_BACK;
        strcpy(page.items[3].label, "BACK TO NES OPTIONS");
    } else {
        static const char *const labels[] = {
            "", "SAVE STATE", "LOAD STATE", "DELETE STATE", "SCREENSHOT",
            "RESTART GAME", "VIDEO OPTIONS", "BACK TO GAME MENU"
        };
        page.count = 8;
        page.back_action = NES_SHARED_BACK;
        for (int i = 0; i < 8; ++i) {
            page.items[i].action = NES_SHARED_SLOT + i;
            snprintf(page.items[i].label, sizeof(page.items[i].label), "%s", labels[i]);
        }
        page.items[0].flags = PAX_GAME_MENU_ITEM_ADJUSTABLE;
        snprintf(page.items[0].label, sizeof(page.items[0].label), "STATE SLOT %d: %s",
                 nes_shared.slot, nes_shared_slot_exists() ? "SAVED" : "EMPTY");
        if (nes_shared.thumb && nes_read_preview(nes_shared.svc, nes_shared.rom_hash,
                                                nes_shared.slot, nes_shared.thumb) == 0)
            page.preview = nes_shared.thumb;
    }
    nes_shared.svc->game_menu_set_page(&page);
}

static void nes_shared_action(void *context, uint32_t action, int32_t direction)
{
    (void)context;
    const app_services_t *svc = nes_shared.svc;
    const char *status = NULL;
    int selected = action >= NES_SHARED_SLOT && action <= NES_SHARED_BACK ?
                   (int)action - NES_SHARED_SLOT : 0;
    const int confirmed = nes_shared.confirm_action == (int)action;
    nes_shared.confirm_action = 0;
    if (svc->log_printf)
        svc->log_printf("NES shared menu: action=%lu slot=%d direction=%ld\n",
                        (unsigned long)action, nes_shared.slot, (long)direction);
    switch (action) {
        case PAX_GAME_MENU_ACTION_OPEN:
            nes_shared.video = 0;
            break;
        case NES_SHARED_SLOT:
            nes_shared.slot = (nes_shared.slot + (direction < 0 ? -1 : 1) + NES_STATE_SLOTS) % NES_STATE_SLOTS;
            break;
        case NES_SHARED_SAVE:
            if (nes_shared_slot_exists() && !confirmed) {
                nes_shared.confirm_action = action;
                status = "A AGAIN TO OVERWRITE";
            } else {
                status = nes_save_slot(svc, nes_shared.rom_hash, nes_shared.slot, nes_shared.frame) == 0 ?
                         "STATE SAVED" : "SAVE FAILED";
            }
            break;
        case NES_SHARED_LOAD: {
            const int result = nes_load_slot(svc, nes_shared.rom_hash, nes_shared.slot);
            if (result == 0) {
                svc->game_menu_close();
                return;
            }
            if (result != -2)
                nes_reset(HARD_RESET);
            status = result == -2 ? "EMPTY SLOT" : "BAD STATE RESET";
            break;
        }
        case NES_SHARED_DELETE:
            if (!nes_shared_slot_exists()) {
                status = "EMPTY SLOT";
            } else if (!confirmed) {
                nes_shared.confirm_action = action;
                status = "A AGAIN TO DELETE";
            } else {
                status = nes_delete_slot(svc, nes_shared.rom_hash, nes_shared.slot) == 0 ?
                         "STATE DELETED" : "DELETE FAILED";
            }
            break;
        case NES_SHARED_SCREENSHOT:
            status = nes_save_screenshot(svc, nes_shared.rom_hash, nes_shared.frame) == 0 ?
                     "SCREENSHOT SAVED" : "SCREENSHOT FAILED";
            break;
        case NES_SHARED_RESTART:
            nes_reset(HARD_RESET);
            svc->game_menu_close();
            return;
        case NES_SHARED_VIDEO:
            nes_shared.video = 1;
            selected = 0;
            break;
        case NES_SHARED_BACK:
            svc->game_menu_set_page(NULL);
            return;
        case NES_SHARED_PALETTE:
        case NES_SHARED_CROP:
        case NES_SHARED_ZOOM:
            selected = (int)action - NES_SHARED_PALETTE;
            status = nes_change_option(svc, nes_shared.rom_hash, selected, direction < 0 ? -1 : 1) == 0 ?
                     "OPTION SAVED" : "OPTION SAVE FAILED";
            break;
        case NES_SHARED_VIDEO_BACK:
            nes_shared.video = 0;
            selected = 6;
            break;
        default:
            return;
    }
    nes_shared_publish(selected, status);
}

int pax_nes_shared_menu_attach(const app_services_t *svc, const uint16_t *game_frame,
                                uint16_t width, uint16_t height)
{
    if (!svc || !svc->game_menu_register || !svc->game_menu_set_page || !svc->game_menu_close ||
        width != NES_MENU_W || height != NES_MENU_H)
        return 0;
    nes_shared.frame = game_frame;
    if (!nes_shared.registered) {
        nes_shared.svc = svc;
        nes_shared.rom_hash = nes_rom_hash(svc);
        if (svc->mem_caps_alloc)
            nes_shared.thumb = svc->mem_caps_alloc(NES_THUMB_W * NES_THUMB_H * sizeof(uint16_t), PAX_MEM_CAP_SPIRAM);
        nes_shared.registered = svc->game_menu_register("NES OPTIONS", nes_shared_action, NULL) == 0;
        if (!nes_shared.registered && nes_shared.thumb && svc->mem_free) {
            svc->mem_free(nes_shared.thumb);
            nes_shared.thumb = NULL;
        }
    }
    return nes_shared.registered;
}

void pax_nes_shared_menu_detach(void)
{
    if (nes_shared.registered)
        nes_shared.svc->game_menu_register(NULL, NULL, NULL);
    if (nes_shared.thumb && nes_shared.svc->mem_free)
        nes_shared.svc->mem_free(nes_shared.thumb);
    memset(&nes_shared, 0, sizeof(nes_shared));
}

int pax_nes_menu_swipe_down_pressed(const app_services_t *svc)
{
    if (!svc || !svc->touch_read) {
        nes_touch_active = 0;
        nes_touch_swipe_fired = 0;
        return 0;
    }

    int x = 0, y = 0;
    if (!svc->touch_read(&x, &y)) {
        nes_touch_active = 0;
        nes_touch_swipe_fired = 0;
        return 0;
    }

    (void)x;
    if (!nes_touch_active) {
        nes_touch_active = 1;
        nes_touch_swipe_fired = 0;
        nes_touch_start_y = y;
        return 0;
    }

    /* Match the launcher-style pull-down gesture: begin near the top edge
       and move down far enough to avoid treating taps as a menu request. */
    if (!nes_touch_swipe_fired && nes_touch_start_y <= 100 &&
        y - nes_touch_start_y >= 80) {
        nes_touch_swipe_fired = 1;
        return 1;
    }
    return 0;
}

int pax_nes_menu_run(const app_services_t *svc, const uint16_t *game_frame,
                      uint16_t width, uint16_t height)
{
    if (svc && svc->log_printf)
        svc->log_printf("NES PAX: menu runner entered\n");
    if (!svc || !svc->input_gamepad_read || !svc->display_write_frame_custom ||
        !svc->mem_caps_alloc || !svc->mem_free) {
        if (svc && svc->log_printf)
            svc->log_printf("NES PAX: menu runner missing required service\n");
        return 0;
    }

    const size_t frame_bytes = (size_t)NES_MENU_W * NES_MENU_H * sizeof(uint16_t);
    uint16_t *menu_frame = (uint16_t *)svc->mem_caps_alloc(
        frame_bytes, PAX_MEM_CAP_SPIRAM | PAX_MEM_CAP_DMA);
    uint16_t *thumb = (uint16_t *)svc->mem_caps_alloc(
        NES_THUMB_W * NES_THUMB_H * sizeof(uint16_t), PAX_MEM_CAP_SPIRAM);
    if (!menu_frame || !thumb) {
        if (svc->log_printf)
            svc->log_printf("NES PAX: menu framebuffer allocation failed\n");
        if (menu_frame) svc->mem_free(menu_frame);
        if (thumb) svc->mem_free(thumb);
        return 0;
    }
    memset(thumb, 0, NES_THUMB_W * NES_THUMB_H * sizeof(uint16_t));

    const uint32_t rom_hash = nes_rom_hash(svc);
    if (rom_hash != nes_loaded_rom_hash)
        pax_nes_options_load(svc, rom_hash);
    int page = NES_PAGE_ROOT;
    int selected = 0;
    int slot = 0;
    int scroll = 0;
    int confirm_slot = -1;
    const char *message = NULL;
    int message_frames = 0;
    bool menu_active = true;
    bool exit_game = false;
    pax_gamepad_state_t previous = {0};
    pax_gamepad_state_t current = {0};

    /* Drain the launcher selection too (usually A), not only the synthetic
       MENU pulse. Otherwise a held confirm can immediately choose RESUME. */
    int release_polls = 0;
    for (int i = 0; i < 50; ++i) {
        svc->input_gamepad_read(&current);
        ++release_polls;
        const bool any_button_down =
            current.values[PAX_INPUT_MENU] || current.values[PAX_INPUT_A] ||
            current.values[PAX_INPUT_B] || current.values[PAX_INPUT_SELECT] ||
            current.values[PAX_INPUT_START] || current.values[PAX_INPUT_UP] ||
            current.values[PAX_INPUT_DOWN] || current.values[PAX_INPUT_LEFT] ||
            current.values[PAX_INPUT_RIGHT] || current.values[PAX_INPUT_X] ||
            current.values[PAX_INPUT_Y] || current.values[PAX_INPUT_L] ||
            current.values[PAX_INPUT_R];
        if (!any_button_down)
            break;
        if (svc->delay_ms) svc->delay_ms(10);
    }
    previous = current;
    if (svc->log_printf)
        svc->log_printf("NES PAX: menu input drained after %d polls (A=%d B=%d MENU=%d)\n",
                        release_polls, current.values[PAX_INPUT_A],
                        current.values[PAX_INPUT_B], current.values[PAX_INPUT_MENU]);

    int traced_frames = 0;
    while (menu_active) {
        if (pax_exit_requested ||
            (svc->input_l3_read && svc->input_l3_read())) {
            exit_game = true;
            break;
        }

        nes_draw_menu(svc, game_frame, menu_frame, width, height, thumb, rom_hash,
                      page, selected, scroll, confirm_slot,
                      message_frames > 0 ? message : NULL);
        if (traced_frames == 0 && svc->log_printf)
            svc->log_printf("NES PAX: root/options menu frame submitted\n");
        if (svc->delay_ms) svc->delay_ms(80);
        svc->input_gamepad_read(&current);
        if (traced_frames < 3 && svc->log_printf)
            svc->log_printf("NES PAX: menu poll %d up=%d down=%d A=%d B=%d MENU=%d prevA=%d\n",
                            traced_frames, current.values[PAX_INPUT_UP],
                            current.values[PAX_INPUT_DOWN], current.values[PAX_INPUT_A],
                            current.values[PAX_INPUT_B], current.values[PAX_INPUT_MENU],
                            previous.values[PAX_INPUT_A]);
        ++traced_frames;

        const bool lr_exit = current.values[PAX_INPUT_L] && current.values[PAX_INPUT_R];
        if (lr_exit) {
            exit_game = true;
            break;
        }

        if (current.values[PAX_INPUT_UP] && !previous.values[PAX_INPUT_UP]) {
            if (page == NES_PAGE_ROOT)
                selected = (selected + NES_MENU_ROOT_COUNT - 1) % NES_MENU_ROOT_COUNT;
            else if (page == NES_PAGE_OPTIONS)
                selected = (selected + NES_OPTION_COUNT - 1) % NES_OPTION_COUNT;
            else {
                slot = (slot + NES_STATE_SLOTS - 1) % NES_STATE_SLOTS;
                selected = slot;
                scroll = selected >= NES_MENU_VISIBLE_SLOTS ?
                         selected - NES_MENU_VISIBLE_SLOTS + 1 : 0;
            }
            confirm_slot = -1;
        }
        if (current.values[PAX_INPUT_DOWN] && !previous.values[PAX_INPUT_DOWN]) {
            if (page == NES_PAGE_ROOT)
                selected = (selected + 1) % NES_MENU_ROOT_COUNT;
            else if (page == NES_PAGE_OPTIONS)
                selected = (selected + 1) % NES_OPTION_COUNT;
            else {
                slot = (slot + 1) % NES_STATE_SLOTS;
                selected = slot;
                scroll = selected >= NES_MENU_VISIBLE_SLOTS ?
                         selected - NES_MENU_VISIBLE_SLOTS + 1 : 0;
            }
            confirm_slot = -1;
        }

        /* MENU/Capture belongs to the launcher's shared overlay. If the user
           opens that overlay while this modal is active and chooses NES
           OPTIONS again, do not treat the synthetic MENU pulse as Back and
           silently drop them onto a still-paused game. B is the explicit
           Back/Resume control within the NES menu. */
        const bool back = current.values[PAX_INPUT_B] && !previous.values[PAX_INPUT_B];
        if (back) {
            if (page == NES_PAGE_ROOT)
                menu_active = false;
            else {
                page = NES_PAGE_ROOT;
                selected = 0;
                confirm_slot = -1;
            }
        }

        if (current.values[PAX_INPUT_A] && !previous.values[PAX_INPUT_A]) {
            if (page == NES_PAGE_ROOT) {
                switch (selected) {
                    case NES_MENU_RESUME:
                        menu_active = false;
                        break;
                    case NES_MENU_SAVE:
                        page = NES_PAGE_SAVE;
                        selected = slot = scroll = 0;
                        confirm_slot = -1;
                        message = NULL;
                        break;
                    case NES_MENU_LOAD:
                        page = NES_PAGE_LOAD;
                        selected = slot = scroll = 0;
                        confirm_slot = -1;
                        message = NULL;
                        break;
                    case NES_MENU_DELETE:
                        page = NES_PAGE_DELETE;
                        selected = slot = scroll = 0;
                        confirm_slot = -1;
                        message = NULL;
                        break;
                    case NES_MENU_SCREENSHOT:
                        message = nes_save_screenshot(svc, rom_hash, game_frame) == 0 ?
                                  "SCREENSHOT SAVED" : "SCREENSHOT FAILED";
                        message_frames = 30;
                        break;
                    case NES_MENU_RESTART:
                        nes_reset(HARD_RESET);
                        menu_active = false;
                        break;
                    case NES_MENU_OPTIONS:
                        page = NES_PAGE_OPTIONS;
                        selected = 0;
                        confirm_slot = -1;
                        message = NULL;
                        break;
                    case NES_MENU_EXIT:
                        exit_game = true;
                        menu_active = false;
                        break;
                }
            } else if (page == NES_PAGE_OPTIONS) {
                const int result = nes_change_option(svc, rom_hash, selected, 1);
                message = result == 0 ? "OPTION SAVED" : "OPTION SAVE FAILED";
                message_frames = 20;
            } else if (page == NES_PAGE_SAVE) {
                char state[128], preview[128], temp[128], backup[128];
                const bool exists = nes_state_paths(rom_hash, selected, state, sizeof(state),
                    preview, sizeof(preview), temp, sizeof(temp), backup, sizeof(backup)) == 0 &&
                    nes_path_exists(svc, state);
                if (exists && confirm_slot != selected) {
                    confirm_slot = selected;
                    message = NULL;
                } else {
                    const int result = nes_save_slot(svc, rom_hash, selected, game_frame);
                    message = result == 0 ? "STATE SAVED" : "SAVE FAILED";
                    message_frames = 20;
                    confirm_slot = -1;
                }
            } else if (page == NES_PAGE_LOAD) {
                const int result = nes_load_slot(svc, rom_hash, selected);
                if (result == 0) {
                    menu_active = false;
                } else {
                    if (result != -2)
                        nes_reset(HARD_RESET);
                    message = result == -2 ? "EMPTY SLOT" : "BAD STATE RESET";
                    message_frames = 20;
                }
            } else if (page == NES_PAGE_DELETE) {
                char state[128], preview[128], temp[128], backup[128];
                const bool exists = nes_state_paths(rom_hash, selected, state, sizeof(state),
                    preview, sizeof(preview), temp, sizeof(temp), backup, sizeof(backup)) == 0 &&
                    nes_path_exists(svc, state);
                if (!exists) {
                    message = "EMPTY SLOT";
                    message_frames = 20;
                } else if (confirm_slot != selected) {
                    confirm_slot = selected;
                    message = NULL;
                } else {
                    const int result = nes_delete_slot(svc, rom_hash, selected);
                    message = result == 0 ? "STATE DELETED" : "DELETE FAILED";
                    message_frames = 20;
                    confirm_slot = -1;
                }
            }
        }

        if (page == NES_PAGE_OPTIONS) {
            int direction = 0;
            if (current.values[PAX_INPUT_LEFT] && !previous.values[PAX_INPUT_LEFT])
                direction = -1;
            if (current.values[PAX_INPUT_RIGHT] && !previous.values[PAX_INPUT_RIGHT])
                direction = 1;
            if (direction != 0) {
                const int result = nes_change_option(svc, rom_hash, selected, direction);
                message = result == 0 ? "OPTION SAVED" : "OPTION SAVE FAILED";
                message_frames = 20;
            }
        }

        if (message_frames > 0)
            --message_frames;
        previous = current;
    }

    svc->mem_free(thumb);
    svc->mem_free(menu_frame);
    if (svc->log_printf)
        svc->log_printf("NES PAX: menu runner leaving page=%d selection=%d exit=%d\n",
                        page, selected, exit_game);
    return exit_game ? 1 : 0;
}
