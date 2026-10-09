#include "../pax_files.c"
#include <assert.h>

const app_services_t *_pax_svc;
static int made, removed;
static int list(const char *path, char *out, int length) {
    static const char entries[] = "base1.sav\0base1.sv2\0.base2.sav\0folder/\0base2.sav\0server.ssv\0";
    assert(!strcmp(path, "/sd/save/current"));
    assert(length >= (int)sizeof entries);
    memcpy(out, entries, sizeof entries);
    return 6;
}
static int make(const char *path) { assert(!strcmp(path, "/sd/save/new")); ++made; return 0; }
static int erase(const char *path) { assert(!strcmp(path, "/sd/save/new/server.ssv")); ++removed; return 0; }
static int info(const char *path, pax_file_stat_t *out) {
    if (!strcmp(path, "/sd/save/current")) {
        *out = (pax_file_stat_t){.is_dir = 1}; return 0;
    }
    if (!strcmp(path, "/sd/save/new/server.ssv")) {
        *out = (pax_file_stat_t){.size = 1024, .mtime = 123456}; return 0;
    }
    return -1;
}
int main(void) {
    static const app_services_t service = {
        .file_list_dir = list, .file_mkdir = make, .file_remove = erase, .file_stat = info
    };
    _pax_svc = &service;
    assert(match_name("*.sav", "base1.sav"));
    assert(match_name("base?.s*", "base1.sav"));
    assert(match_name("*base*1*", "xxbase11"));
    assert(!match_name("*.sav", "base1.sv2"));
    assert(!match_name("base?", "base"));
    assert(!strcmp(Sys_FindFirst("/sd/save/current/*.sav", 0, SFF_HIDDEN | SFF_SUBDIR),
                   "/sd/save/current/base1.sav"));
    assert(!strcmp(Sys_FindNext(0, SFF_HIDDEN | SFF_SUBDIR), "/sd/save/current/base2.sav"));
    assert(!Sys_FindNext(0, SFF_HIDDEN | SFF_SUBDIR));
    Sys_FindClose(); assert(!Sys_FindNext(0, 0));
    assert(!strcmp(Sys_FindFirst("/sd/save/current/*", SFF_SUBDIR, 0), "/sd/save/current/folder"));
    assert(!Sys_FindNext(SFF_SUBDIR, 0));
    assert(!strcmp(Sys_FindFirst("/sd/save/current/server.ssv", 0, 0), "/sd/save/current/server.ssv"));
    assert(!Sys_FindFirst(NULL, 0, 0));
    assert(!Sys_FindFirst("invalid", 0, 0));
    Sys_Mkdir("/sd/save/current"); assert(!made);
    Sys_Mkdir("/sd/save/new"); assert(made == 1);
    assert(!pax_file_mkdir("/sd/save/new") && made == 2);
    assert(!pax_file_remove("/sd/save/new/server.ssv") && removed == 1);
    struct stat st;
    assert(!pax_file_stat("/sd/save/current", &st) && S_ISDIR(st.st_mode));
    assert(!pax_file_stat("/sd/save/new/server.ssv", &st) && S_ISREG(st.st_mode));
    assert(st.st_size == 1024 && st.st_mtime == 123456);
    assert(pax_file_stat("missing", &st) == -1 && errno == ENOENT);
    static const app_services_t old_service = {0};
    _pax_svc = &old_service;
    assert(!Sys_FindFirst("/sd/save/current/*", 0, 0));
    assert(pax_file_mkdir("/sd/save/new") == -1 && errno == ENOSYS);
    assert(pax_file_remove("/sd/save/new/server.ssv") == -1 && errno == ENOSYS);
    puts("PASS: save-directory creation, glob listing/filtering, stat/remove and missing-service errors");
}
