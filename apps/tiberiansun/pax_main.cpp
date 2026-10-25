// Tiberian Sun PAX port: staged startup and MIX data layer.
//
// The desktop OpenTS executable still depends on Win32 and bgfx, so the P4
// port is being brought over in layers.  This first engine layer implements
// the same MIX directory semantics that OpenTS uses and verifies real startup
// resources from the card before the configuration and renderer layers land.
#include "pax_port.h"
#include "ts_data.h"
#include "ts_ini.h"
#include "ts_mix.h"

#include <string.h>

extern "C" {
const app_services_t *pax_svc = nullptr;

long long pax_time_us(void)
{
    return pax_svc != nullptr && pax_svc->get_time_us != nullptr ?
        pax_svc->get_time_us() : 0;
}

int pax_socket_close(int fd)
{
    (void)fd;
    return -1;
}

void pax_quit(int code)
{
    if (pax_svc != nullptr && pax_svc->log_printf != nullptr) {
        pax_svc->log_printf("TS: quit requested (%d)\\n", code);
    }
    for (;;) {
        if (pax_svc != nullptr && pax_svc->delay_ms != nullptr) {
            pax_svc->delay_ms(1000);
        }
    }
}
}

struct file_check {
    const char *name;
    bool required;
};

static const file_check s_files[] = {
    {"tibsun.mix", true},
    {"maps01.mix", true},
    {"multi.mix", true},
    {"scores.mix", true},
    {"maps02.mix", false},
    {"maps03.mix", false},
    {"movies01.mix", false},
    {"movies02.mix", false},
    {"movies03.mix", false},
    {"sidecd01.mix", false},
    {"sidecd02.mix", false},
    {"expand01.mix", false},
    {"scores01.mix", false},
    {"e01scd01.mix", false},
    {"e01scd02.mix", false}
};

struct ts_runtime_state {
    uint32_t archives;
    uint32_t scenarios;
    uint32_t terrain_packs;
    bool rules;
    bool art;
    bool keyboard;
    bool firestorm;
};

static ts_runtime_state s_runtime = {};

static void make_path(char *out, const char *name)
{
    const char *prefix = PAX_TS_DATA_DIR "/";
    unsigned i = 0;
    while (prefix[i] != '\0') {
        out[i] = prefix[i];
        ++i;
    }
    while (*name != '\0') {
        out[i++] = *name++;
    }
    out[i] = '\0';
}

static void check_one(const file_check &item)
{
    char path[128];
    make_path(path, item.name);
    void *file = pax_svc->file_open(path, "rb");
    if (file == nullptr) {
        pax_svc->log_printf("TS: %s %s\n", item.required ? "MISSING" : "optional missing", path);
        return;
    }
    pax_svc->file_seek(file, 0, 2 /* SEEK_END */);
    const long size = pax_svc->file_tell(file);
    pax_svc->file_close(file);
    pax_svc->log_printf("TS: found %s (%ld bytes)%s\n", path, size,
                         item.required ? " [required]" : " [optional]");
}

static void probe_archive(const char *name, bool required)
{
    char path[128];
    make_path(path, name);
    pax_svc->log_printf("TS MIX: probing %s\n", path);
    TsMixArchive archive;
    if (!archive.open(path)) {
        pax_svc->log_printf("TS MIX: %s %s (%s)\n", required ? "MISSING" : "optional missing", path,
                             archive.error() != nullptr ? archive.error() : "invalid archive");
        return;
    }

    pax_svc->log_printf("TS MIX: opened %s entries=%lu data=%lu start=%lu\n", path,
                         static_cast<unsigned long>(archive.count()),
                         static_cast<unsigned long>(archive.data_size()),
                         static_cast<unsigned long>(archive.data_start()));

    // These are among the first files OpenTS asks for during its desktop
    // bootstrap.  A hit proves the CRC and binary-search rules match MIX.
    static const char *const startup_files[] = {
        "TIBSUN.INI", "RULES.INI", "ART.INI", "CONQUER.INI", "TS_TITLE.VQA", "MOUSE.SHP",
    };
    for (const char *startup : startup_files) {
        ts_mix_entry entry{};
        pax_svc->log_printf("TS MIX: %s %s\n", startup, archive.find(startup, &entry) ? "found" : "not found");
    }
}

static bool probe_terrain_pack(const TsIniDocument &scenario, const char *name)
{
    static const char *const sections[] = {
        "IsoMapPack5", "IsoMapPack4", "IsoMapPack3", "IsoMapPack2", "IsoMapPack"
    };
    for (const char *section : sections) {
        const size_t lines = scenario.entry_count(section);
        if (lines == 0) continue;

        // OpenTS bounds each decoded terrain revision at 512 KiB. Keep this
        // temporary block in PSRAM and release it before the next mission.
        const size_t capacity = 512U * 1024U;
        uint8_t *payload = static_cast<uint8_t *>(pax_svc->mem_alloc(capacity));
        if (payload == nullptr) {
            pax_svc->log_printf("TS MAP: %s %s lines=%lu decode buffer unavailable\n", name, section,
                                 static_cast<unsigned long>(lines));
            return false;
        }
        size_t decoded = 0;
        const bool decoded_ok = scenario.decode_base64_section(section, payload, capacity, &decoded);
        bool blocks_ok = decoded_ok;
        unsigned blocks = 0;
        size_t at = 0;
        while (blocks_ok && at < decoded) {
            if (decoded - at < 4) {
                blocks_ok = false;
                break;
            }
            const uint16_t compressed = static_cast<uint16_t>(payload[at]) |
                                        static_cast<uint16_t>(payload[at + 1]) << 8;
            const uint16_t uncompressed = static_cast<uint16_t>(payload[at + 2]) |
                                          static_cast<uint16_t>(payload[at + 3]) << 8;
            if (compressed == 0 || uncompressed == 0 || uncompressed > 8192 || compressed > 16384 ||
                compressed > decoded - at - 4) {
                blocks_ok = false;
                break;
            }
            at += 4 + compressed;
            ++blocks;
        }
        const bool valid = blocks_ok && at == decoded && decoded != 0;
        pax_svc->log_printf("TS MAP: %s %s lines=%lu decoded=%lu blocks=%u framing=%s\n", name, section,
                             static_cast<unsigned long>(lines), static_cast<unsigned long>(decoded), blocks,
                             valid ? "ok" : "invalid");
        pax_svc->mem_free(payload);
        return valid;
    }
    return false;
}

static void probe_data_store()
{
    TsDataStore store;
    // Keep the order used by OpenTS: patches and numbered expansion archives
    // shadow the shipped/base archives, while maps and multiplayer content are
    // mounted later and remain optional.
    static const char *const archives[] = {
        "patch.mix", "expand03.mix", "expand02.mix", "expand01.mix",
        "tibsun.mix", "cache.mix", "conquer.mix", "sounds.mix", "scores.mix",
        "movies03.mix", "movies02.mix", "movies01.mix",
        "scores01.mix", "maps03.mix", "maps02.mix", "maps01.mix", "multi.mix",
        "sidecd02.mix", "sidecd01.mix", "e01scd02.mix", "e01scd01.mix",
    };
    for (const char *name : archives) {
        if (store.mount(name)) {
            pax_svc->log_printf("TS FS: mounted %s\n", name);
        }
    }
    s_runtime.archives = store.archive_count();
    pax_svc->log_printf("TS FS: mounted %lu MIX archives\n",
                         static_cast<unsigned long>(s_runtime.archives));

    // These are representative files requested during OpenTS bootstrap.  The
    // lookup path is now the same one the eventual configuration and engine
    // layers will use: loose SD data first, then mounted archives.
    static const char *const startup_files[] = {
        "OPENTS.INI", "CONQUER.INI", "RULES.INI", "ART.INI",
        "KEYBOARD.INI", "SOUND.INI", "MOUSE.SHP", "FIRESTRM.INI",
    };
    for (const char *name : startup_files) {
        ts_data_ref ref{};
        const bool found = store.find(name, &ref);
        pax_svc->log_printf("TS FS: %s %s\n", name,
                             found ? store.source_name(ref) : "missing");
    }

    // Load the first configuration files through the same store.  This is the
    // seam where the desktop OpenTS initialization can be brought over without
    // making the game depend on the ESP32's POSIX filesystem.
    static const char *const config_files[] = {"RULES.INI", "ART.INI", "KEYBOARD.INI", "FIRESTRM.INI"};
    for (const char *name : config_files) {
        TsIniDocument ini;
        if (!ini.load(store, name)) {
            continue;
        }
        if (strcmp(name, "RULES.INI") == 0) s_runtime.rules = true;
        if (strcmp(name, "ART.INI") == 0) s_runtime.art = true;
        if (strcmp(name, "KEYBOARD.INI") == 0) s_runtime.keyboard = true;
        if (strcmp(name, "FIRESTRM.INI") == 0) s_runtime.firestorm = true;
        pax_svc->log_printf("TS CFG: loaded %s (%lu bytes)\n", name,
                             static_cast<unsigned long>(ini.size()));
        char value[64];
        if (ini.get("General", "Name", value, sizeof(value))) {
            pax_svc->log_printf("TS CFG: %s [General] Name=%s\n", name, value);
        }
        if (ini.has_section("Firestorm")) {
            pax_svc->log_printf("TS CFG: %s has [Firestorm]\n", name);
        }
    }

    // OpenTS discovers campaigns and mission descriptions through the same
    // data layer.  The full campaign selector is a later UI/engine step, but
    // resolving the known stock names here catches both an incorrect MIX
    // precedence and an incorrectly named card layout before we get there.
    static const char *const scenario_files[] = {
        "BATTLE.INI", "MAPSEL.INI", "MAPSEL01.INI",
        "GDI01EA.INI", "GDI02EA.INI", "GDI03EA.INI", "GDI04EA.INI", "GDI05EA.INI",
        "GDI01A.INI", "GDI02A.INI", "GDI03A.INI", "GDI04A.INI", "GDI05A.INI",
        "NOD01EA.INI", "NOD02EA.INI", "NOD03EA.INI", "NOD04EA.INI", "NOD05EA.INI",
        "NOD01A.INI", "NOD02A.INI", "NOD03A.INI", "NOD04A.INI", "NOD05A.INI",
        "SCG01EA.INI", "SCU01EA.INI", "SCB01EA.INI"
    };
    unsigned discovered = 0;
    unsigned terrain_probed = 0;
    for (const char *name : scenario_files) {
        ts_data_ref ref{};
        if (!store.find(name, &ref)) continue;
        ++discovered;
        TsIniDocument scenario;
        char title[96] = {};
        char theater[32] = {};
        if (scenario.load(store, name)) {
            scenario.get("Basic", "Name", title, sizeof(title));
            scenario.get("Map", "Theater", theater, sizeof(theater));
        }
        pax_svc->log_printf("TS SCEN: %s source=%s title=%s theater=%s\n", name,
                             store.source_name(ref), title, theater);
    }
    pax_svc->log_printf("TS SCEN: discovered %u stock campaign/config files\n", discovered);

    // MAPSEL01.INI stores the mission filename in numbered entries under the
    // house sections. Read those names directly instead of assuming a CD's
    // historical naming convention.
    TsIniDocument mapsel;
    if (mapsel.load(store, "MAPSEL01.INI")) {
        static const char *const sections[] = {"GDI", "NOD", "CABAL", "ALLIED", "SOVIET", "Firestorm"};
        static const char *const numbers[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12"};
        for (const char *section : sections) {
            for (const char *number : numbers) {
                char value[128] = {};
                if (mapsel.get(section, number, value, sizeof(value))) {
                    pax_svc->log_printf("TS SCEN: MAPSEL01 [%s] %s=%s\n", section, number, value);
                    char scenario_name[128] = {};
                    if (mapsel.get(value, "Scenario", scenario_name, sizeof(scenario_name))) {
                        ts_data_ref scenario_ref{};
                        const bool scenario_found = store.find(scenario_name, &scenario_ref);
                        if (scenario_found) ++s_runtime.scenarios;
                        char scenario_title[128] = {};
                        char scenario_theater[32] = {};
                        char scenario_size[64] = {};
                        char scenario_local_size[64] = {};
                        size_t units = 0;
                        size_t infantry = 0;
                        size_t aircraft = 0;
                        size_t structures = 0;
                        size_t triggers = 0;
                        size_t iso_pack = 0;
                        size_t iso_pack2 = 0;
                        size_t iso_pack3 = 0;
                        size_t iso_pack4 = 0;
                        size_t iso_pack5 = 0;
                        bool terrain_valid = false;
                        if (scenario_found) {
                            TsIniDocument scenario;
                            if (scenario.load(store, scenario_name)) {
                                scenario.get("Basic", "Name", scenario_title, sizeof(scenario_title));
                                scenario.get("Map", "Theater", scenario_theater, sizeof(scenario_theater));
                                scenario.get("Map", "Size", scenario_size, sizeof(scenario_size));
                                scenario.get("Map", "LocalSize", scenario_local_size, sizeof(scenario_local_size));
                                units = scenario.entry_count("Units");
                                infantry = scenario.entry_count("Infantry");
                                aircraft = scenario.entry_count("Aircraft");
                                structures = scenario.entry_count("Structures");
                                triggers = scenario.entry_count("Triggers");
                                iso_pack = scenario.entry_count("IsoMapPack");
                                iso_pack2 = scenario.entry_count("IsoMapPack2");
                                iso_pack3 = scenario.entry_count("IsoMapPack3");
                                iso_pack4 = scenario.entry_count("IsoMapPack4");
                                iso_pack5 = scenario.entry_count("IsoMapPack5");
                                // Boot validation samples a few packs across
                                // the campaign. The remaining terrain payloads
                                // are intentionally deferred until a mission
                                // is selected so startup stays responsive.
                                if (terrain_probed < 3) {
                                    terrain_valid = probe_terrain_pack(scenario, scenario_name);
                                    ++terrain_probed;
                                }
                            }
                        }
                        pax_svc->log_printf("TS SCEN: stage %s Scenario=%s source=%s\n", value,
                                             scenario_name,
                                             scenario_found ? store.source_name(scenario_ref) : "missing");
                        if (scenario_found) {
                            if (terrain_valid) {
                                ++s_runtime.terrain_packs;
                            }
                            pax_svc->log_printf("TS SCEN: %s title=%s theater=%s map=%s local=%s objects U=%lu I=%lu A=%lu S=%lu T=%lu terrain=%lu/%lu/%lu/%lu/%lu\n",
                                                 scenario_name, scenario_title, scenario_theater, scenario_size, scenario_local_size,
                                                 static_cast<unsigned long>(units), static_cast<unsigned long>(infantry),
                                                 static_cast<unsigned long>(aircraft), static_cast<unsigned long>(structures),
                                                 static_cast<unsigned long>(triggers), static_cast<unsigned long>(iso_pack),
                                                 static_cast<unsigned long>(iso_pack2), static_cast<unsigned long>(iso_pack3),
                                                 static_cast<unsigned long>(iso_pack4), static_cast<unsigned long>(iso_pack5));
                        }
                    }
                }
            }
        }
    }
}

// ── Small runtime shell ────────────────────────────────────────────────────
//
// This is deliberately independent of LVGL. It exercises the same framebuffer
// and close services that the eventual OpenTS renderer will use, while keeping
// the data/configuration milestone visible on the actual panel instead of
// returning to the launcher immediately after startup.

struct ts_glyph {
    char ch;
    uint8_t row[7];
};

static const ts_glyph s_font[] = {
    {' ', {0, 0, 0, 0, 0, 0, 0}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'3', {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}},
    {'4', {0x12, 0x12, 0x12, 0x1F, 0x02, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0F}},
    {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
};

static const ts_glyph *find_glyph(char ch)
{
    for (const ts_glyph &glyph : s_font) {
        if (glyph.ch == ch) return &glyph;
    }
    return &s_font[0];
}

static uint16_t rgb565(uint8_t red, uint8_t green, uint8_t blue)
{
    return static_cast<uint16_t>(((red & 0xF8) << 8) |
                                 ((green & 0xFC) << 3) |
                                 (blue >> 3));
}

static void fill_rect(uint16_t *fb, int width, int height, int x, int y, int w, int h, uint16_t color)
{
    if (fb == nullptr || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > width) w = width - x;
    if (y + h > height) h = height - y;
    if (w <= 0 || h <= 0) return;
    for (int row = 0; row < h; ++row) {
        uint16_t *line = fb + (y + row) * width + x;
        for (int col = 0; col < w; ++col) line[col] = color;
    }
}

static int text_width(const char *text, int scale)
{
    int width = 0;
    for (const char *at = text; at != nullptr && *at != '\0'; ++at) {
        width += (*at == ' ' ? 4 : 6) * scale;
    }
    return width > 0 ? width - scale : 0;
}

static void draw_text(uint16_t *fb, int width, int height, int x, int y,
                      const char *text, int scale, uint16_t color)
{
    if (fb == nullptr || text == nullptr || scale <= 0) return;
    int at_x = x;
    for (const char *at = text; *at != '\0'; ++at) {
        const ts_glyph *glyph = find_glyph(*at);
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if ((glyph->row[row] & (1u << (4 - col))) == 0) continue;
                fill_rect(fb, width, height, at_x + col * scale, y + row * scale,
                          scale, scale, color);
            }
        }
        at_x += (*at == ' ' ? 4 : 6) * scale;
    }
}

static void draw_centered(uint16_t *fb, int width, int height, int y,
                          const char *text, int scale, uint16_t color)
{
    draw_text(fb, width, height, (width - text_width(text, scale)) / 2, y,
              text, scale, color);
}

static bool runtime_close_requested(const app_services_t *svc)
{
    if (svc->input_l3_read != nullptr && svc->input_l3_read()) return true;
    if (svc->input_gamepad_read != nullptr) {
        pax_gamepad_state_t pad{};
        svc->input_gamepad_read(&pad);
        return pad.values[PAX_INPUT_MENU] && pad.values[PAX_INPUT_X];
    }
    return false;
}

static void render_runtime_screen(const app_services_t *svc, int width, int height)
{
    if (svc->display_get_framebuffer == nullptr || svc->display_flush == nullptr) return;
    uint16_t *fb = svc->display_get_framebuffer();
    if (fb == nullptr || width <= 0 || height <= 0) return;

    const uint16_t bg = rgb565(7, 11, 18);
    const uint16_t panel = rgb565(17, 26, 38);
    const uint16_t cyan = rgb565(83, 210, 226);
    const uint16_t white = rgb565(235, 242, 248);
    const uint16_t muted = rgb565(153, 171, 188);
    const uint16_t green = rgb565(83, 211, 131);
    const uint16_t amber = rgb565(237, 183, 74);

    if (svc->display_lock != nullptr) svc->display_lock();
    fill_rect(fb, width, height, 0, 0, width, height, bg);
    fill_rect(fb, width, height, 0, 0, width, 86, panel);
    fill_rect(fb, width, height, 0, 82, width, 4, cyan);
    draw_centered(fb, width, height, 18, "TIBERIAN SUN", 4, white);
    draw_centered(fb, width, height, 54, "OPEN TS P4", 2, cyan);

    const int left = width > 600 ? (width - 560) / 2 : 24;
    const int box_w = width > 600 ? 560 : width - 48;
    const int box_h = 38;
    const int gap = 10;
    const int top = height > 420 ? 124 : 104;
    const char *labels[] = {"TERRAIN MAPS OK", "MIX ARCHIVES OK", "RULES INI OK", "ART INI OK",
                            "KEYBOARD INI OK", "FIRESTRM INI OK"};
    const bool ready[] = {s_runtime.scenarios != 0 && s_runtime.terrain_packs != 0,
                          s_runtime.archives != 0, s_runtime.rules,
                          s_runtime.art, s_runtime.keyboard, s_runtime.firestorm};
    for (unsigned i = 0; i < sizeof(labels) / sizeof(labels[0]); ++i) {
        const int y = top + static_cast<int>(i) * (box_h + gap);
        fill_rect(fb, width, height, left, y, box_w, box_h, panel);
        fill_rect(fb, width, height, left, y, 7, box_h, ready[i] ? green : amber);
        draw_text(fb, width, height, left + 24, y + 10, labels[i], 2, ready[i] ? white : amber);
    }
    draw_centered(fb, width, height, height - 48, "HOLD L3 TO EXIT", 2, muted);
    if (svc->display_unlock != nullptr) svc->display_unlock();
    svc->display_flush();
}

static void run_runtime_session(const app_services_t *svc)
{
    int width = 800;
    int height = 480;
    if (svc->display_get_size != nullptr) svc->display_get_size(&width, &height);
    // The PAX framebuffer has a stride equal to the active canvas width. Some
    // older loaders report the physical panel here until an app selects its
    // own canvas, while the screenshot stream remains the ABI 800x480 canvas.
    // Select that canvas explicitly so the rows we draw and the rows captured
    // by the loader cannot disagree.
    if (svc->display_set_canvas != nullptr && svc->display_set_canvas(800, 480) == 0) {
        width = 800;
        height = 480;
    }
    render_runtime_screen(svc, width, height);
    svc->log_printf("TS: runtime shell ready; waiting for close request\n");
    while (!runtime_close_requested(svc)) {
        if (svc->delay_ms != nullptr) svc->delay_ms(20);
    }
    svc->log_printf("TS: runtime shell closed\n");
    if (svc->display_clear != nullptr && svc->display_flush != nullptr) {
        svc->display_clear(0x0000);
        svc->display_flush();
    }
}

static int ts_run(const app_services_t *svc)
{
    pax_svc = svc;
    svc->log_printf("TS: tiberiansun.pax started\n");
    svc->log_printf("TS: loader ABI %lu\n", static_cast<unsigned long>(svc->abi_version));

    int width = 0;
    int height = 0;
    if (svc->display_get_size != nullptr) {
        svc->display_get_size(&width, &height);
        svc->log_printf("TS: canvas %dx%d\n", width, height);
    }
    if (svc->display_clear != nullptr && svc->display_flush != nullptr) {
        svc->display_clear(0x0000);
        svc->display_flush();
    }

    for (const file_check &item : s_files) {
        check_one(item);
    }
    probe_archive("tibsun.mix", true);
    probe_archive("maps01.mix", true);
    probe_archive("multi.mix", true);
    probe_data_store();
    svc->log_printf("TS: MIX/data/config layer finished\n");
    run_runtime_session(svc);
    return 0;
}

// MIX probing uses large local index structures. Keep them off the loader's
// small internal entry stack and let the loader own this PSRAM task.
static volatile bool ts_done;
static int ts_result;
static void ts_task(void *) {
    ts_result = ts_run(pax_svc);
    ts_done = true;
    for (;;) pax_svc->delay_ms(1000);
}
extern "C" __attribute__((section(".text.entry"), used))
int app_entry(const app_services_t *svc) {
    if (!svc || svc->abi_version != PAX_ABI_VERSION) return -1;
    pax_svc = svc;
    ts_done = false;
    ts_result = -1;
    void *task = nullptr;
    if (!svc->task_create || svc->task_create(ts_task, "tiberiansun", 65536,
            nullptr, 5, &task, 0) != 0) {
        svc->log_printf("TS: data task creation failed\n");
        return -1;
    }
    while (!ts_done) svc->delay_ms(20);
    svc->task_delete(task);
    return ts_result;
}
