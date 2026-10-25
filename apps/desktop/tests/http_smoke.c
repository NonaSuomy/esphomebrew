/* HTTP command path: bounded credentials, partial writes, HTTP errors and
 * cancellation. */
#define PAX_APP_SIDE 1
#include "../relay_http.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char *configuration, *response;
static char sent[4096];
static size_t offset, sent_used;
static int64_t now;
static unsigned reads, sends, closed, pumps;
static int cancel;
static void *open_file(const char *path, const char *mode) {
  assert(!strcmp(path, "/sd/roms/desktop/relay.conf"));
  assert(!strcmp(mode, "rb"));
  return configuration ? (void *)1 : 0;
}
static size_t read_file(void *out, size_t size, size_t count, void *file) {
  assert(file == (void *)1 && size == 1);
  size_t n = strlen(configuration);
  assert(n < count);
  memcpy(out, configuration, n);
  return n;
}
static int close_file(void *file) {
  assert(file == (void *)1);
  return 0;
}
static int resolve(const char *name, uint32_t *ip) {
  assert(!strcmp(name, "ha.test"));
  *ip = 0x7f000001;
  return 1;
}
static int connect_socket(uint32_t ip, uint16_t port) {
  assert(ip == 0x7f000001 && port == 8123);
  return 3;
}
static int poll_socket(int handle) {
  assert(handle == 3);
  return 2;
}
static int send_socket(int handle, const void *data, int n) {
  assert(handle == 3);
  if (++sends % 4 == 0)
    return 0;
  if (n > 7)
    n = 7;
  assert(sent_used + (size_t)n < sizeof sent);
  memcpy(sent + sent_used, data, (size_t)n);
  sent_used += (size_t)n;
  sent[sent_used] = 0;
  return n;
}
static int recv_socket(int handle, void *out, int n) {
  assert(handle == 3 && n == 1);
  if (++reads % 3 == 0)
    return -2;
  if (!response[offset])
    return 0;
  *(char *)out = response[offset++];
  return 1;
}
static void close_socket(int handle) {
  assert(handle == 3);
  closed++;
}
static int64_t time_now(void) { return now; }
static void delay(int ms) { now += (int64_t)ms * 1000; }
static int pump(void) {
  pumps++;
  return cancel;
}
int main(void) {
  app_services_t api = {0};
  api.file_open = open_file;
  api.file_read = read_file;
  api.file_close = close_file;
  api.mem_alloc = malloc;
  api.mem_free = free;
  api.net_resolve = resolve;
  api.net_tcp_connect = connect_socket;
  api.net_poll = poll_socket;
  api.net_tcp_send = send_socket;
  api.net_tcp_recv = recv_socket;
  api.net_udp_close = close_socket;
  api.get_time_us = time_now;
  api.delay_ms = delay;
  const unsigned codes[] = {200, 401, 404, 502, 403};
  for (unsigned i = 0; i < 5; i++) {
    char line[64];
    snprintf(line, sizeof line, "HTTP/1.1 %u Reply\r\n", codes[i]);
    response = line;
    configuration = "http://ha.test:8123\nTEST_ONLY_TOKEN\n";
    offset = sent_used = reads = sends = closed = pumps = 0;
    now = 0;
    const char *error =
        relay_start_vnc(&api, "computer.test", 5901, "p\"ass\\", pump);
    assert((i == 0) == (error == 0));
    assert(closed == 1 && pumps > 20);
    assert(strstr(sent, "POST /api/svp_relay/desktop HTTP/1.1\r\n"));
    assert(strstr(sent, "Authorization: Bearer TEST_ONLY_TOKEN\r\n"));
    assert(strstr(sent, "\"password\":\"p\\\"ass\\\\\""));
  }
  configuration = 0;
  assert(relay_start_vnc(&api, "pc", 5900, "", pump));
  configuration = "https://ha.test:8123\nTEST_ONLY_TOKEN\n";
  assert(relay_start_vnc(&api, "pc", 5900, "", pump));
  configuration = "http://ha.test:8123\nTEST_ONLY_TOKEN\n";
  cancel = 1;
  assert(relay_start_vnc(&api, "pc", 5900, "", pump));
  puts("Relay HTTP partial IO, JSON quoting, status errors, missing config, "
       "TLS capability and cancellation passed");
  return 0;
}
