#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
namespace pax_firmware {
// Activate only a complete, hash-checked ESP image. The target is always the
// inactive app slot; its end() additionally validates the ESP image header.
template <class Source, class Target, class Digest>
bool transfer(Source &source, Target &target, Digest &digest, uint32_t size,
              const char *expected) {
  if (!size || !expected || !expected[0] || !target.begin(size))
    return false;
  uint8_t buffer[4096];
  uint32_t done = 0;
  bool ok = digest.begin();
  while (ok && done < size) {
    size_t wanted = std::min(sizeof(buffer), size_t(size - done));
    int count = source.read(buffer, wanted);
    ok = count > 0 && size_t(count) <= wanted && digest.update(buffer, count) &&
         target.write(buffer, count);
    if (ok) {
      done += count;
      target.progress(done, size);
    }
  }
  ok = ok && done == size && digest.matches(expected);
  if (!ok) {
    target.abort();
    return false;
  }
  if (!target.end())
    return false;
  return target.activate();
}
} // namespace pax_firmware
