#pragma once
#include <stdbool.h>
#include <string.h>
/* These formats match each active emulator adapter's emu_rom_path/load_rom. */
typedef struct { const char *id, *title, *extensions; } pax_rom_profile_t;
static const pax_rom_profile_t pax_rom_profiles[] = {
    {"nes", "NES", ".nes|"},
    {"nesplus", "NES Plus", ".nes|.fds|.nsf|"},
    {"gb", "Game Boy", ".gb|.gbc|"},
    {"sms", "Master System / Game Gear", ".sms|.gg|"},
    {"genesis", "Genesis / Mega Drive", ".md|.gen|.bin|.smd|"},
    {"snes", "Super Nintendo", ".smc|.sfc|"},
    {"pce", "PC Engine", ".pce|"},
    {"stella", "Atari 2600", ".a26|.bin|"},
    {"prosystem", "Atari 7800", ".a78|.bin|"},
    {"atari800", "Atari 800 / 5200", ".atr|.xex|.com|.cas|.a52|.car|.rom|.bin|"},
    {"handy", "Atari Lynx", ".lnx|.lyx|"},
    {"ngp", "Neo Geo Pocket", ".ngp|.ngc|"},
    {"ws", "WonderSwan", ".ws|.wsc|"},
    {"msx", "MSX", ".rom|.mx1|.mx2|"},
    {"gx4000", "GX4000", ".cpr|.bin|"},
    {"gw", "Game and Watch", ".gw|.lcd|"},
    {"c64", "Commodore 64", ".d64|.t64|"},
    {"spectrum", "ZX Spectrum", ".z80|.sna|.tap|.tzx|"},
    {"neogeo", "Neo Geo", ".zip|"},
    {"azip", "Z-machine", ".dat|.z1|.z2|.z3|.z4|.z5|.z6|.z7|.z8|"},
};
static const pax_rom_profile_t *pax_rom_profile_for_app(const char *path)
{
    if (!path) return NULL;
    const char *base=path;
    for (const char *p=path; *p; ++p) if (*p=='/') base=p+1;
    size_t length=strlen(base);
    if (length<4 || length>=64) return NULL;
    char stem[64];
    for (size_t i=0;i<=length;++i) stem[i]=base[i]>='A' && base[i]<='Z' ? base[i]+('a'-'A') : base[i];
    if (strcmp(stem+length-4,".pax")) return NULL;
    stem[length-4]=0;
    length-=4;
    if (length>=6 && !strcmp(stem+length-6,"-debug")) stem[length-6]=0;
    for (size_t i=0;i<sizeof(pax_rom_profiles)/sizeof(pax_rom_profiles[0]);++i)
        if (!strcmp(stem,pax_rom_profiles[i].id)) return &pax_rom_profiles[i];
    return NULL;
}
static bool pax_rom_profile_accepts(const pax_rom_profile_t *profile, const char *name)
{
    if (!profile || !name) return false;
    const char *ext=NULL;
    for (const char *p=name; *p; ++p) if (*p=='.') ext=p;
    if (!ext || strlen(ext)>16) return false;
    char token[19];size_t n=strlen(ext);
    for (size_t i=0;i<n;++i) token[i]=ext[i]>='A' && ext[i]<='Z' ? ext[i]+('a'-'A') : ext[i];
    token[n]='|';token[n+1]=0;
    return !strcmp(token,".zip|") || strstr(profile->extensions,token)!=NULL;
}
