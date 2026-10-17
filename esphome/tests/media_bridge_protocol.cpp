#include "../components/pax_media_bridge/protocol.h"
#include <cassert>
#include <iostream>
using namespace esphome::pax_media_bridge;
int main() {
  Header h;
  assert(parse_header("POST /browse HTTP/1.1\r\nContent-Length: 2\r\n\r\n{}",
                      h) == 1 &&
         h.length == 2);
  assert(parse_header("GET / HTTP/1.1\r\n", h) == 0);
  for (auto s :
       {"POST / HTTP/1.1\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\n",
        "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n",
        "POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n",
        "POST / HTTP/1.1\r\nContent-Length: 4097\r\n\r\n"})
    assert(parse_header(s, h) == -1);
  assert(parse_header(std::string(4097, 'x'), h) == -1);
  uint64_t a, b;
  assert(byte_range("", 100, a, b) && a == 0 && b == 99);
  assert(byte_range("bytes=10-20", 100, a, b) && a == 10 && b == 20);
  assert(byte_range("bytes=90-", 100, a, b) && a == 90 && b == 99);
  assert(byte_range("bytes=-12", 100, a, b) && a == 88 && b == 99);
  assert(byte_range("bytes=-120", 100, a, b) && a == 0 && b == 99);
  for (auto s : {"bytes=100-", "bytes=20-10", "bytes=1-2,4-5", "bytes=-0",
                 "bytes=18446744073709551616-", "bytes=x-y"})
    assert(!byte_range(s, 100, a, b));
  assert(!byte_range("", 0, a, b));
  assert(safe_media_path("/sd/media/folder/Track.MP3"));
  for (auto s : {"/sd/media/../secret.mp3", "/sd/media//x.mp4",
                 "/sd/media/./x.mp3", "/sd/roms/x.mp3", "/sd/media/x.yaml",
                 "/sd/media/x\\y.mp3", "/sd/media/http:x.mp3"})
    assert(!safe_media_path(s));
  assert(json_quote("a\"\n\\") == "\"a\\\"\\u000a\\\\\"");
  std::cout << "Bridge HTTP framing, ranges, path boundaries and JSON quoting "
               "passed\n";
}
