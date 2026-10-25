// Small OpenTS-compatible MIX archive reader for the staged PAX port.
//
// OpenTS's MixFileClass keeps the directory in memory and reads payloads on
// demand.  This class follows that same layout, but uses only the loader ABI
// so it can run before the desktop file layer and Win32 startup code are ported.
#pragma once

#include <stddef.h>
#include <stdint.h>

struct ts_mix_entry {
    uint32_t crc;
    uint32_t offset;
    uint32_t size;
};

class TsMixArchive {
public:
    TsMixArchive();
    ~TsMixArchive();

    TsMixArchive(const TsMixArchive &) = delete;
    TsMixArchive &operator=(const TsMixArchive &) = delete;

    bool open(const char *path);
    void close();
    bool find(const char *name, ts_mix_entry *out) const;
    bool read(const char *name, void *buffer, size_t capacity, size_t *out_size = nullptr) const;

    const char *error() const { return error_; }
    uint32_t count() const { return count_; }
    uint32_t data_size() const { return data_size_; }
    uint32_t data_start() const { return data_start_; }

private:
    bool read_exact(void *buffer, size_t bytes) const;
    bool read_entry(const ts_mix_entry &entry, void *buffer, size_t capacity, size_t *out_size) const;
    void set_error(const char *error) { error_ = error; }

    void *file_;
    ts_mix_entry *entries_;
    uint32_t count_;
    uint32_t data_size_;
    uint32_t data_start_;
    uint32_t file_size_;
    const char *error_;
    char path_[160];
};

