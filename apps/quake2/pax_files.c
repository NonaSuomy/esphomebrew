/* Directory operations needed by Quake II's autosave/copy/load path. */
#include "psram_app.h"
#include "qcommon/qcommon.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

extern const app_services_t *_pax_svc;
static char find_entries[16384];
static char find_dir[MAX_OSPATH], find_pattern[MAX_OSPATH], find_path[MAX_OSPATH];
static size_t find_offset;
static int find_active;

static int match_name(const char *pattern, const char *name) {
    const char *star = NULL, *retry = NULL;
    while (*name) {
        if (*pattern == '?' || *pattern == *name) { ++pattern; ++name; }
        else if (*pattern == '*') { star = pattern++; retry = name; }
        else if (star) { pattern = star + 1; name = ++retry; }
        else return 0;
    }
    while (*pattern == '*') ++pattern;
    return !*pattern;
}

void Sys_Mkdir(char *path) {
    pax_file_stat_t existing;
    if (_pax_svc && _pax_svc->file_stat &&
        _pax_svc->file_stat(path, &existing) == 0 && existing.is_dir) return;
    if (!_pax_svc || !_pax_svc->file_mkdir || _pax_svc->file_mkdir(path) != 0) {
        if (_pax_svc && _pax_svc->log_printf)
            _pax_svc->log_printf("QUAKE2 WARN: cannot create directory %s\n", path);
    }
}

void Sys_FindClose(void) { find_active = 0; find_offset = 0; }

char *Sys_FindNext(unsigned musthave, unsigned canthave) {
    while (find_active && find_offset < sizeof find_entries && find_entries[find_offset]) {
        char *name = find_entries + find_offset;
        size_t length = strnlen(name, sizeof find_entries - find_offset);
        if (length == sizeof find_entries - find_offset) break;
        find_offset += length + 1;
        unsigned attributes = name[0] == '.' ? SFF_HIDDEN : 0;
        if (length && name[length - 1] == '/') {
            attributes |= SFF_SUBDIR;
            name[--length] = 0;
        }
        if (!length || (attributes & canthave) || (attributes & musthave) != musthave ||
            !match_name(find_pattern, name)) continue;
        int written = snprintf(find_path, sizeof find_path, "%s/%s", find_dir, name);
        if (written > 0 && written < (int)sizeof find_path) return find_path;
    }
    return NULL;
}

char *Sys_FindFirst(char *path, unsigned musthave, unsigned canthave) {
    Sys_FindClose();
    if (!path || !_pax_svc || !_pax_svc->file_list_dir) return NULL;
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path || slash - path >= (int)sizeof find_dir ||
        strlen(slash + 1) >= sizeof find_pattern) return NULL;
    memcpy(find_dir, path, (size_t)(slash - path));
    find_dir[slash - path] = 0;
    strcpy(find_pattern, slash + 1);
    memset(find_entries, 0, sizeof find_entries);
    if (_pax_svc->file_list_dir(find_dir, find_entries, sizeof find_entries) < 0) return NULL;
    find_active = 1;
    return Sys_FindNext(musthave, canthave);
}

int pax_file_mkdir(const char *path) {
    if (!_pax_svc || !_pax_svc->file_mkdir) { errno = ENOSYS; return -1; }
    if (_pax_svc->file_mkdir(path) == 0) return 0;
    errno = EIO;
    return -1;
}

int pax_file_remove(const char *path) {
    if (!_pax_svc || !_pax_svc->file_remove) { errno = ENOSYS; return -1; }
    if (_pax_svc->file_remove(path) == 0) return 0;
    errno = ENOENT;
    return -1;
}

int pax_file_stat(const char *path, struct stat *st) {
    if (!_pax_svc || !_pax_svc->file_stat) { errno = ENOSYS; return -1; }
    pax_file_stat_t info;
    if (_pax_svc->file_stat(path, &info) != 0) { errno = ENOENT; return -1; }
    memset(st, 0, sizeof(*st));
    st->st_mode = (info.is_dir ? S_IFDIR : S_IFREG) | 0777;
    st->st_size = info.size;
    st->st_mtime = info.mtime;
    return 0;
}
