#pragma once
#include "pax_data.h"
#include <algorithm>

namespace pax_c6 {
// Validate a raw ESP-IDF C6 application, including all segments, checksum and
// appended SHA-256. Factory/merged binaries and standalone BT apps are rejected.
template<class Progress> bool validate_image(FILE *file, uint32_t &size, char *version, size_t capacity, Progress progress) {
  using esphome::pax_loader::data::Sha256;
  if (!file || fseek(file, 0, SEEK_END)) return false;
  long length = ftell(file);
  if (length < 288 || length > 4 * 1024 * 1024 || fseek(file, 0, SEEK_SET)) return false;
  size = static_cast<uint32_t>(length);
  progress(0,size);
  uint8_t header[24];
  if (fread(header, 1, 24, file) != 24 || header[0] != 0xe9 || !header[1] || header[1] > 16 ||
      header[12] != 13 || header[13] != 0 || header[23] != 1) return false;
  Sha256 hash; hash.update(header, 24);
  uint32_t offset = 24; uint8_t checksum = 0xef, buffer[1500];
  for (unsigned segment = 0; segment < header[1]; segment++) {
    uint8_t sh[8];
    if (fread(sh, 1, 8, file) != 8) return false;
    hash.update(sh, 8); offset += 8;
    uint32_t remaining = uint32_t(sh[4]) | uint32_t(sh[5]) << 8 | uint32_t(sh[6]) << 16 | uint32_t(sh[7]) << 24;
    if (offset > size || remaining > size - offset) return false;
    bool first = true;
    while (remaining) {
      size_t n = std::min<size_t>(remaining, sizeof(buffer));
      if (fread(buffer, 1, n, file) != n) return false;
      if (segment == 0 && first) {
        if (n < 256 || buffer[0] != 0x32 || buffer[1] != 0x54 || buffer[2] != 0xcd || buffer[3] != 0xab ||
            !memchr(buffer + 16, 0, 32) || !memchr(buffer + 48, 0, 32) ||
            strcmp(reinterpret_cast<char *>(buffer + 48), "network_adapter")) return false;
        snprintf(version, capacity, "%.31s", reinterpret_cast<char *>(buffer + 16));
      }
      first = false; hash.update(buffer, n);
      for (size_t i = 0; i < n; i++) checksum ^= buffer[i];
      offset += n; remaining -= n;
      progress(offset,size);
    }
    if (segment == 0 && first) return false;
  }
  size_t padding = 16 - (offset % 16); // final byte is the XOR checksum
  if (fread(buffer, 1, padding, file) != padding || buffer[padding - 1] != checksum) return false;
  hash.update(buffer, padding); offset += padding;
  if (offset + 32 != size || fread(buffer, 1, 32, file) != 32) return false;
  static const char hex[] = "0123456789abcdef";
  std::string expected(64, '0');
  for (unsigned i = 0; i < 32; i++) { expected[i*2] = hex[buffer[i] >> 4]; expected[i*2+1] = hex[buffer[i] & 15]; }
  return hash.hex() == expected && fseek(file, 0, SEEK_SET) == 0;
}
inline bool validate_image(FILE *file, uint32_t &size, char *version, size_t capacity) {
  return validate_image(file,size,version,capacity,[](uint32_t,uint32_t){});
}
// A valid ESP image does not identify its transport. Only an approved full-file
// digest binds the contents to the selected C6 transport and pin configuration.
template<class Progress> bool matches_approved_image(FILE *file, const char *expected, Progress progress) {
  if(!file || !expected || strlen(expected)!=64 || fseek(file,0,SEEK_SET))return false;
  esphome::pax_loader::data::Sha256 hash;
  uint8_t buffer[1500];size_t count;uint32_t done=0;
  while((count=fread(buffer,1,sizeof(buffer),file))){hash.update(buffer,count);done+=count;progress(done);}
  bool ok=!ferror(file) && hash.hex()==expected;
  return fseek(file,0,SEEK_SET)==0 && ok;
}
inline bool matches_approved_image(FILE *file,const char *expected){
  return matches_approved_image(file,expected,[](uint32_t){});
}
// Transport returns 0 on success. Finalize an opened handle on failure, but
// never activate an incomplete transfer. Kept portable for fault-injection tests.
template<class Transport> int transfer_image(FILE *file, uint32_t size, Transport &transport, uint32_t &sent) {
  sent = 0;
  int error = transport.begin();
  if (error) return error;
  uint8_t buffer[1500];
  while (!error && sent < size) {
    size_t n = std::min<size_t>(sizeof(buffer), size - sent);
    if (fread(buffer, 1, n, file) != n) { error = -1; break; }
    error = transport.write(buffer, n);
    if (!error) { sent += n; transport.progress(sent, size); }
  }
  int final_error = transport.end();
  if (!error) error = final_error;
  return error ? error : transport.activate();
}

}
