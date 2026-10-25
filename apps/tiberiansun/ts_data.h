// OpenTS-style data layer for the staged Tiberian Sun PAX.
//
// Loose files take precedence over mounted MIX archives, matching the desktop
// engine's file layer.  Archive indexes stay in PSRAM and member payloads are
// read on demand from the SD card.
#pragma once

#include "ts_mix.h"

#include <stddef.h>
#include <stdint.h>

struct ts_data_ref {
    bool loose;
    uint32_t archive_index;
    ts_mix_entry entry;
    char name[96];
};

class TsDataStore {
public:
    TsDataStore();
    ~TsDataStore();

    TsDataStore(const TsDataStore &) = delete;
    TsDataStore &operator=(const TsDataStore &) = delete;

    void close();
    bool mount(const char *name);
    bool find(const char *name, ts_data_ref *out) const;
    bool read(const ts_data_ref &ref, void *buffer, size_t capacity, size_t *out_size = nullptr) const;

    uint32_t archive_count() const { return archive_count_; }
    const char *archive_name(uint32_t index) const;
    const char *source_name(const ts_data_ref &ref) const;

private:
    static constexpr uint32_t MAX_ARCHIVES = 24;
    TsMixArchive archives_[MAX_ARCHIVES];
    char archive_names_[MAX_ARCHIVES][96];
    uint32_t archive_count_;
};
