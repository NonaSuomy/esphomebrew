// Small case-insensitive INI reader for the staged OpenTS configuration layer.
#pragma once

#include "ts_data.h"

#include <stddef.h>
#include <stdint.h>

class TsIniDocument {
public:
    TsIniDocument();
    ~TsIniDocument();

    TsIniDocument(const TsIniDocument &) = delete;
    TsIniDocument &operator=(const TsIniDocument &) = delete;

    bool load(const TsDataStore &store, const char *name);
    bool has_section(const char *section) const;
    bool get(const char *section, const char *key, char *out, size_t capacity) const;
    size_t entry_count(const char *section) const;
    bool decode_base64_section(const char *section, uint8_t *out, size_t capacity,
                               size_t *out_size) const;
    size_t size() const { return size_; }

private:
    char *data_;
    size_t size_;
};
