#include "ts_data.h"

#include "pax_port.h"

#include <string.h>

namespace {

static void make_path(char *out, size_t capacity, const char *name)
{
    const char *prefix = PAX_TS_DATA_DIR "/";
    size_t at = 0;
    while (*prefix != '\0' && at + 1 < capacity) {
        out[at++] = *prefix++;
    }
    while (name != nullptr && *name != '\0' && at + 1 < capacity) {
        out[at++] = *name++;
    }
    out[at] = '\0';
}

static bool read_file(void *file, void *buffer, size_t bytes)
{
    uint8_t *cursor = static_cast<uint8_t *>(buffer);
    while (bytes != 0) {
        const size_t got = pax_svc->file_read(cursor, 1, bytes, file);
        if (got == 0) {
            return false;
        }
        cursor += got;
        bytes -= got;
    }
    return true;
}

}  // namespace

TsDataStore::TsDataStore() : archives_{}, archive_names_{}, archive_count_(0)
{
}

TsDataStore::~TsDataStore()
{
    close();
}

void TsDataStore::close()
{
    for (uint32_t i = 0; i < archive_count_; ++i) {
        archives_[i].close();
        archive_names_[i][0] = '\0';
    }
    archive_count_ = 0;
}

bool TsDataStore::mount(const char *name)
{
    if (name == nullptr || name[0] == '\0' || archive_count_ >= MAX_ARCHIVES ||
        pax_svc == nullptr || pax_svc->file_open == nullptr) {
        return false;
    }

    char path[160];
    make_path(path, sizeof(path), name);
    TsMixArchive &archive = archives_[archive_count_];
    if (!archive.open(path)) {
        return false;
    }
    strncpy(archive_names_[archive_count_], name, sizeof(archive_names_[0]) - 1);
    archive_names_[archive_count_][sizeof(archive_names_[0]) - 1] = '\0';
    ++archive_count_;
    return true;
}

bool TsDataStore::find(const char *name, ts_data_ref *out) const
{
    if (name == nullptr || name[0] == '\0' || out == nullptr || pax_svc == nullptr ||
        pax_svc->file_open == nullptr || pax_svc->file_close == nullptr) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    strncpy(out->name, name, sizeof(out->name) - 1);
    out->name[sizeof(out->name) - 1] = '\0';

    // OpenTS gives a loose file priority over every archive member.  This is
    // also how OPENTS.INI and user overrides remain editable on the SD card.
    char path[160];
    make_path(path, sizeof(path), name);
    void *file = pax_svc->file_open(path, "rb");
    if (file != nullptr) {
        out->loose = true;
        out->archive_index = UINT32_MAX;
        if (pax_svc->file_seek != nullptr && pax_svc->file_tell != nullptr &&
            pax_svc->file_seek(file, 0, 2) == 0) {
            const long size = pax_svc->file_tell(file);
            if (size >= 0 && static_cast<unsigned long>(size) <= 0xFFFFFFFFUL) {
                out->entry.size = static_cast<uint32_t>(size);
            }
        }
        pax_svc->file_close(file);
        return true;
    }

    for (uint32_t i = 0; i < archive_count_; ++i) {
        if (archives_[i].find(name, &out->entry)) {
            out->loose = false;
            out->archive_index = i;
            return true;
        }
    }
    return false;
}

bool TsDataStore::read(const ts_data_ref &ref, void *buffer, size_t capacity, size_t *out_size) const
{
    if (out_size != nullptr) {
        *out_size = 0;
    }
    if (buffer == nullptr || pax_svc == nullptr || pax_svc->file_open == nullptr ||
        pax_svc->file_close == nullptr || pax_svc->file_read == nullptr) {
        return false;
    }

    if (!ref.loose) {
        if (ref.archive_index >= archive_count_) {
            return false;
        }
        return archives_[ref.archive_index].read(ref.name, buffer, capacity, out_size);
    }

    char path[160];
    make_path(path, sizeof(path), ref.name);
    void *file = pax_svc->file_open(path, "rb");
    if (file == nullptr || ref.entry.size > capacity) {
        if (file != nullptr) {
            pax_svc->file_close(file);
        }
        return false;
    }
    const bool ok = read_file(file, buffer, ref.entry.size);
    pax_svc->file_close(file);
    if (ok && out_size != nullptr) {
        *out_size = ref.entry.size;
    }
    return ok;
}

const char *TsDataStore::archive_name(uint32_t index) const
{
    return index < archive_count_ ? archive_names_[index] : nullptr;
}

const char *TsDataStore::source_name(const ts_data_ref &ref) const
{
    if (ref.loose) {
        return "loose";
    }
    return archive_name(ref.archive_index);
}
