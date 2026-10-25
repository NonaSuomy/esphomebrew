#include "ts_mix.h"

#include "pax_port.h"
#include "ts_crypto.h"

#include <string.h>

namespace {

static uint32_t crc_table[256];
static bool crc_ready = false;

static void init_crc_table()
{
    if (crc_ready) {
        return;
    }
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t value = i;
        for (unsigned bit = 0; bit < 8; ++bit) {
            value = (value & 1U) ? (value >> 1) ^ 0xEDB88320U : value >> 1;
        }
        crc_table[i] = value;
    }
    crc_ready = true;
}

static uint32_t crc_memory(const uint8_t *data, size_t length, uint32_t crc)
{
    init_crc_table();
    crc ^= 0xFFFFFFFFU;
    while (length-- != 0) {
        crc = crc_table[(crc ^ *data++) & 0xFFU] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFU;
}

// OpenTS uses CRCEngine for MIX names.  Complete four-byte groups are fed to
// CRC::Memory directly; a short final group gets its historical padding.
static uint32_t filename_crc(const char *name)
{
    uint8_t upper[256];
    size_t length = 0;
    while (name != nullptr && name[length] != '\0' && length + 1 < sizeof(upper)) {
        const unsigned char c = static_cast<unsigned char>(name[length]);
        upper[length] = (c >= 'a' && c <= 'z') ? static_cast<uint8_t>(c - 'a' + 'A') : c;
        ++length;
    }

    uint32_t crc = 0;
    const size_t complete = length & ~static_cast<size_t>(3);
    if (complete != 0) {
        crc = crc_memory(upper, complete, crc);
    }
    const size_t remainder = length - complete;
    if (remainder != 0) {
        uint8_t padded[4] = {0, 0, 0, 0};
        for (size_t i = 0; i < remainder; ++i) {
            padded[i] = upper[complete + i];
        }
        padded[remainder] = static_cast<uint8_t>(remainder);
        for (size_t i = remainder + 1; i < sizeof(padded); ++i) {
            padded[i] = padded[0];
        }
        crc = crc_memory(padded, sizeof(padded), crc);
    }
    return crc;
}

static uint16_t le16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(p[1]) << 8;
}

static uint32_t le32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) |
           static_cast<uint32_t>(p[1]) << 8 |
           static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}

static bool signed_less(uint32_t left, uint32_t right)
{
    // MIX directories are ordered using OpenTS's signed int comparison.
    return static_cast<int32_t>(left) < static_cast<int32_t>(right);
}

}  // namespace

TsMixArchive::TsMixArchive()
    : file_(nullptr), entries_(nullptr), count_(0), data_size_(0), data_start_(0), file_size_(0),
      error_(nullptr), path_{}
{
}

TsMixArchive::~TsMixArchive()
{
    close();
}

void TsMixArchive::close()
{
    if (file_ != nullptr && pax_svc != nullptr && pax_svc->file_close != nullptr) {
        pax_svc->file_close(file_);
    }
    if (entries_ != nullptr && pax_svc != nullptr && pax_svc->mem_free != nullptr) {
        pax_svc->mem_free(entries_);
    }
    file_ = nullptr;
    entries_ = nullptr;
    count_ = 0;
    data_size_ = 0;
    data_start_ = 0;
    file_size_ = 0;
    path_[0] = '\0';
}

bool TsMixArchive::read_exact(void *buffer, size_t bytes) const
{
    if (file_ == nullptr || pax_svc == nullptr || pax_svc->file_read == nullptr) {
        return false;
    }
    uint8_t *cursor = static_cast<uint8_t *>(buffer);
    while (bytes != 0) {
        const size_t got = pax_svc->file_read(cursor, 1, bytes, file_);
        if (got == 0) {
            return false;
        }
        cursor += got;
        bytes -= got;
    }
    return true;
}

bool TsMixArchive::open(const char *path)
{
    close();
    error_ = nullptr;
    if (pax_svc == nullptr || pax_svc->file_open == nullptr || pax_svc->file_seek == nullptr ||
        pax_svc->file_tell == nullptr || pax_svc->mem_alloc == nullptr) {
        set_error("loader file services unavailable");
        return false;
    }
    if (path == nullptr || path[0] == '\0') {
        set_error("empty path");
        return false;
    }
    strncpy(path_, path, sizeof(path_) - 1);
    path_[sizeof(path_) - 1] = '\0';

    file_ = pax_svc->file_open(path_, "rb");
    if (file_ == nullptr) {
        set_error("file not found");
        return false;
    }
    if (pax_svc->file_seek(file_, 0, 2) != 0) {
        set_error("could not seek to end");
        close();
        return false;
    }
    const long file_size = pax_svc->file_tell(file_);
    if (file_size < 6 || static_cast<unsigned long>(file_size) > 0xFFFFFFFFUL) {
        set_error("file is too small or too large");
        close();
        return false;
    }
    file_size_ = static_cast<uint32_t>(file_size);
    if (pax_svc->file_seek(file_, 0, 0) != 0) {
        set_error("could not seek to header");
        close();
        return false;
    }

    // Read only the four-byte alternate header first.  Encrypted MIX files
    // put the RSA-wrapped Blowfish key immediately after these four bytes;
    // consuming the first two bytes of the following stream here would shift
    // every encrypted key and make the decrypted directory look random.
    uint8_t alternate[4] = {};
    if (!read_exact(alternate, sizeof(alternate))) {
        set_error("short MIX header");
        close();
        return false;
    }

    const uint16_t first = le16(alternate);
    uint32_t directory_start = 0;
    bool encrypted = false;
    if (first == 0) {
        const uint16_t flags = le16(alternate + 2);
        encrypted = (flags & 0x02U) != 0;
        if (encrypted) {
            // PKStraw uses the 320-bit OpenTS fast key: two 40-byte RSA
            // blocks contain the 56-byte per-archive Blowfish key.
            uint8_t encrypted_key[80] = {};
            uint8_t blowfish_key[56] = {};
            if (!read_exact(encrypted_key, sizeof(encrypted_key)) ||
                !ts_decrypt_mix_key(encrypted_key, sizeof(encrypted_key), blowfish_key)) {
                set_error("could not decrypt MIX key");
                close();
                return false;
            }
            uint8_t first_block[8] = {};
            if (!read_exact(first_block, sizeof(first_block)) ||
                !ts_decrypt_mix_blocks(blowfish_key, first_block, sizeof(first_block))) {
                set_error("could not decrypt MIX header");
                close();
                return false;
            }
            count_ = le16(first_block);
            data_size_ = le32(first_block + 2);
            directory_start = 84;  // 4-byte flags + 80-byte RSA-wrapped key.
            if (pax_svc->log_printf != nullptr) {
                pax_svc->log_printf("TS MIX: header %s encrypted flags=%u count=%lu data=%lu\n", path_,
                                     static_cast<unsigned>(flags), static_cast<unsigned long>(count_),
                                     static_cast<unsigned long>(data_size_));
            }

            if (count_ == 0 || count_ > 200000U) {
                set_error("invalid encrypted MIX entry count");
                close();
                return false;
            }
            const uint64_t directory_bytes = static_cast<uint64_t>(count_) * sizeof(ts_mix_entry);
            const uint64_t encrypted_directory_bytes = (6U + directory_bytes + 7U) & ~7ULL;
            const uint64_t data_start = directory_start + encrypted_directory_bytes;
            if (data_start > file_size_) {
                set_error("encrypted MIX directory exceeds file");
                close();
                return false;
            }
            entries_ = static_cast<ts_mix_entry *>(pax_svc->mem_alloc(count_ * sizeof(ts_mix_entry)));
            if (entries_ == nullptr) {
                set_error("not enough app memory for MIX directory");
                close();
                return false;
            }
            // The first encrypted block already supplied the header and the
            // first two directory bytes.
            uint8_t *directory = reinterpret_cast<uint8_t *>(entries_);
            memcpy(directory, first_block + 6, 2);
            size_t copied = 2;
            uint64_t remaining = encrypted_directory_bytes - sizeof(first_block);
            uint8_t block[8] = {};
            while (remaining != 0) {
                if (!read_exact(block, sizeof(block)) ||
                    !ts_decrypt_mix_blocks(blowfish_key, block, sizeof(block))) {
                    set_error("short encrypted MIX directory");
                    close();
                    return false;
                }
                const size_t take = copied < directory_bytes ?
                    (directory_bytes - copied < sizeof(block) ? directory_bytes - copied : sizeof(block)) : 0;
                if (take != 0) {
                    memcpy(directory + copied, block, take);
                    copied += take;
                }
                remaining -= sizeof(block);
            }
            if (copied != directory_bytes) {
                set_error("short decrypted MIX directory");
                close();
                return false;
            }
            data_start_ = static_cast<uint32_t>(data_start);
        } else {
            // We have already consumed the first four bytes; the extended
            // format's six-byte file header follows them.
            uint8_t tail[6] = {};
            if (!read_exact(tail, sizeof(tail))) {
                set_error("short extended MIX header");
                close();
                return false;
            }
            count_ = le16(tail);
            data_size_ = le32(tail + 2);
            directory_start = 10;
            if (pax_svc->log_printf != nullptr) {
                pax_svc->log_printf("TS MIX: header %s extended flags=%u count=%lu data=%lu\n", path_,
                                     static_cast<unsigned>(flags), static_cast<unsigned long>(count_),
                                     static_cast<unsigned long>(data_size_));
            }
        }
    } else {
        uint8_t tail[2] = {};
        if (!read_exact(tail, sizeof(tail))) {
            set_error("short plain MIX header");
            close();
            return false;
        }
        count_ = first;
        data_size_ = static_cast<uint32_t>(le16(alternate + 2)) |
                     static_cast<uint32_t>(le16(tail)) << 16;
        directory_start = 6;
        if (pax_svc->log_printf != nullptr) {
            pax_svc->log_printf("TS MIX: header %s plain count=%lu data=%lu\n", path_,
                                 static_cast<unsigned long>(count_), static_cast<unsigned long>(data_size_));
        }
    }

    if (!encrypted && (count_ == 0 || count_ > 200000U)) {
        set_error("invalid MIX entry count");
        close();
        return false;
    }
    if (!encrypted) {
        const uint64_t directory_end = static_cast<uint64_t>(directory_start) +
                                        static_cast<uint64_t>(count_) * sizeof(ts_mix_entry);
        if (directory_end > file_size_) {
            set_error("MIX directory exceeds file");
            close();
            return false;
        }
        data_start_ = static_cast<uint32_t>(directory_end);
        entries_ = static_cast<ts_mix_entry *>(pax_svc->mem_alloc(count_ * sizeof(ts_mix_entry)));
        if (entries_ == nullptr) {
            set_error("not enough app memory for MIX directory");
            close();
            return false;
        }
        if (pax_svc->file_seek(file_, static_cast<long>(directory_start), 0) != 0 ||
            !read_exact(entries_, count_ * sizeof(ts_mix_entry))) {
            set_error("short MIX directory");
            close();
            return false;
        }
    }
    const uint64_t data_end = static_cast<uint64_t>(data_start_) + data_size_;
    if (data_end > static_cast<uint64_t>(file_size_) + 20U) {
        set_error("MIX data exceeds file");
        close();
        return false;
    }

    // The loader and the P4 are little-endian, which is also the on-card MIX
    // format. Keep the loop explicit so malformed offsets are rejected here.
    for (uint32_t i = 0; i < count_; ++i) {
        const ts_mix_entry &entry = entries_[i];
        if (static_cast<uint64_t>(entry.offset) + entry.size > data_size_) {
            set_error("MIX entry exceeds data section");
            close();
            return false;
        }
        if (i != 0 && signed_less(entry.crc, entries_[i - 1].crc)) {
            set_error("MIX directory is not sorted");
            close();
            return false;
        }
    }
    return true;
}

bool TsMixArchive::find(const char *name, ts_mix_entry *out) const
{
    if (entries_ == nullptr || name == nullptr) {
        return false;
    }
    const uint32_t key = filename_crc(name);
    uint32_t first = 0;
    uint32_t last = count_;
    while (first < last) {
        const uint32_t middle = first + (last - first) / 2;
        if (entries_[middle].crc == key) {
            if (out != nullptr) {
                *out = entries_[middle];
            }
            return true;
        }
        if (signed_less(entries_[middle].crc, key)) {
            first = middle + 1;
        } else {
            last = middle;
        }
    }
    return false;
}

bool TsMixArchive::read_entry(const ts_mix_entry &entry, void *buffer, size_t capacity, size_t *out_size) const
{
    if (out_size != nullptr) {
        *out_size = 0;
    }
    if (file_ == nullptr || buffer == nullptr || entry.size > capacity ||
        static_cast<uint64_t>(data_start_) + entry.offset + entry.size > file_size_) {
        return false;
    }
    if (pax_svc->file_seek(file_, static_cast<long>(data_start_ + entry.offset), 0) != 0 ||
        !read_exact(buffer, entry.size)) {
        return false;
    }
    if (out_size != nullptr) {
        *out_size = entry.size;
    }
    return true;
}

bool TsMixArchive::read(const char *name, void *buffer, size_t capacity, size_t *out_size) const
{
    ts_mix_entry entry{};
    return find(name, &entry) && read_entry(entry, buffer, capacity, out_size);
}
