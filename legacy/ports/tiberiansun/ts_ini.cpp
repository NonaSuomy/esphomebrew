#include "ts_ini.h"

#include "pax_port.h"

#include <string.h>

namespace {

static bool equal_ci(const char *left, size_t left_len, const char *right)
{
    size_t right_len = 0;
    while (right[right_len] != '\0') {
        ++right_len;
    }
    if (left_len != right_len) {
        return false;
    }
    for (size_t i = 0; i < left_len; ++i) {
        char a = left[i];
        char b = right[i];
        if (a >= 'a' && a <= 'z') a = static_cast<char>(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = static_cast<char>(b - 'a' + 'A');
        if (a != b) {
            return false;
        }
    }
    return true;
}

static const char *skip_space(const char *at, const char *end)
{
    while (at < end && (*at == ' ' || *at == '\t')) {
        ++at;
    }
    return at;
}

static const char *trim_end(const char *begin, const char *end)
{
    while (end > begin && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) {
        --end;
    }
    return end;
}

static int base64_value(char ch)
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

static bool number_key(size_t number, char *out, size_t capacity)
{
    if (out == nullptr || capacity < 2 || number == 0) return false;
    char reversed[20];
    size_t length = 0;
    while (number != 0 && length < sizeof(reversed)) {
        reversed[length++] = static_cast<char>('0' + number % 10);
        number /= 10;
    }
    if (length + 1 > capacity) return false;
    for (size_t i = 0; i < length; ++i) {
        out[i] = reversed[length - i - 1];
    }
    out[length] = '\0';
    return true;
}

template <typename Visitor>
static bool each_line(const char *data, size_t size, Visitor visitor)
{
    const char *at = data;
    const char *finish = data + size;
    while (at < finish) {
        const char *line_end = at;
        while (line_end < finish && *line_end != '\n') {
            ++line_end;
        }
        if (!visitor(at, trim_end(at, line_end))) {
            return false;
        }
        at = line_end < finish ? line_end + 1 : finish;
    }
    return true;
}

}  // namespace

TsIniDocument::TsIniDocument() : data_(nullptr), size_(0)
{
}

TsIniDocument::~TsIniDocument()
{
    if (data_ != nullptr && pax_svc != nullptr && pax_svc->mem_free != nullptr) {
        pax_svc->mem_free(data_);
    }
}

bool TsIniDocument::load(const TsDataStore &store, const char *name)
{
    if (data_ != nullptr && pax_svc != nullptr && pax_svc->mem_free != nullptr) {
        pax_svc->mem_free(data_);
    }
    data_ = nullptr;
    size_ = 0;

    if (pax_svc == nullptr || pax_svc->mem_alloc == nullptr || name == nullptr) {
        return false;
    }
    ts_data_ref ref{};
    if (!store.find(name, &ref) || ref.entry.size > SIZE_MAX - 1) {
        return false;
    }
    data_ = static_cast<char *>(pax_svc->mem_alloc(static_cast<size_t>(ref.entry.size) + 1));
    if (data_ == nullptr) {
        return false;
    }
    size_t got = 0;
    if (!store.read(ref, data_, ref.entry.size, &got)) {
        pax_svc->mem_free(data_);
        data_ = nullptr;
        return false;
    }
    data_[got] = '\0';
    size_ = got;
    return true;
}

bool TsIniDocument::has_section(const char *section) const
{
    if (data_ == nullptr || section == nullptr) {
        return false;
    }
    bool found = false;
    each_line(data_, size_, [&](const char *begin, const char *end) {
        begin = skip_space(begin, end);
        if (begin < end && *begin == '[' && end[-1] == ']') {
            const char *name = begin + 1;
            const char *name_end = trim_end(name, end - 1);
            if (equal_ci(name, static_cast<size_t>(name_end - name), section)) {
                found = true;
                return false;
            }
        }
        return true;
    });
    return found;
}

bool TsIniDocument::get(const char *section, const char *key, char *out, size_t capacity) const
{
    if (data_ == nullptr || section == nullptr || key == nullptr || out == nullptr || capacity == 0) {
        return false;
    }
    out[0] = '\0';
    bool in_section = false;
    bool found = false;
    each_line(data_, size_, [&](const char *begin, const char *end) {
        begin = skip_space(begin, end);
        if (begin == end || *begin == ';' || *begin == '#') {
            return true;
        }
        if (*begin == '[' && end[-1] == ']') {
            const char *name = begin + 1;
            const char *name_end = trim_end(name, end - 1);
            in_section = equal_ci(name, static_cast<size_t>(name_end - name), section);
            return true;
        }
        if (!in_section) {
            return true;
        }
        const char *equals = begin;
        while (equals < end && *equals != '=') {
            ++equals;
        }
        if (equals == end) {
            return true;
        }
        const char *key_end = trim_end(begin, equals);
        const char *value = skip_space(equals + 1, end);
        if (!equal_ci(begin, static_cast<size_t>(key_end - begin), key)) {
            return true;
        }
        const size_t value_len = static_cast<size_t>(end - value) < capacity - 1 ?
            static_cast<size_t>(end - value) : capacity - 1;
        memcpy(out, value, value_len);
        out[value_len] = '\0';
        found = true;
        return false;
    });
    return found;
}

size_t TsIniDocument::entry_count(const char *section) const
{
    if (data_ == nullptr || section == nullptr) {
        return 0;
    }
    bool in_section = false;
    size_t count = 0;
    each_line(data_, size_, [&](const char *begin, const char *end) {
        begin = skip_space(begin, end);
        if (begin == end || *begin == ';' || *begin == '#') {
            return true;
        }
        if (*begin == '[' && end[-1] == ']') {
            const char *name = begin + 1;
            const char *name_end = trim_end(name, end - 1);
            in_section = equal_ci(name, static_cast<size_t>(name_end - name), section);
            return true;
        }
        if (!in_section) {
            return true;
        }
        const char *equals = begin;
        while (equals < end && *equals != '=') {
            ++equals;
        }
        if (equals != end && equals != begin) {
            ++count;
        }
        return true;
    });
    return count;
}

bool TsIniDocument::decode_base64_section(const char *section, uint8_t *out,
                                           size_t capacity, size_t *out_size) const
{
    if (out_size != nullptr) *out_size = 0;
    if (data_ == nullptr || section == nullptr || out == nullptr || out_size == nullptr) {
        return false;
    }

    size_t written = 0;
    uint32_t accumulator = 0;
    unsigned bits = 0;
    const size_t count = entry_count(section);
    for (size_t index = 1; index <= count; ++index) {
        char key[20];
        char encoded[96];
        if (!number_key(index, key, sizeof(key)) || !get(section, key, encoded, sizeof(encoded))) {
            return false;
        }
        for (const char *at = encoded; *at != '\0'; ++at) {
            if (*at == '=' || *at == ' ' || *at == '\t' || *at == '\r') {
                continue;
            }
            const int value = base64_value(*at);
            if (value < 0) return false;
            accumulator = (accumulator << 6) | static_cast<uint32_t>(value);
            bits += 6;
            while (bits >= 8) {
                bits -= 8;
                if (written >= capacity) return false;
                out[written++] = static_cast<uint8_t>((accumulator >> bits) & 0xFFU);
            }
            accumulator = bits == 0 ? 0 : accumulator & ((1U << bits) - 1U);
        }
    }
    *out_size = written;
    return true;
}
