#include "media_client.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static media_listing_t listing;
static char error[MEDIA_ERROR], request[8192], response[4096];
static size_t sent, received;
static int closed;
static int64_t clock_us;
static const char *names[] = {"z.MP3",      "folder",      "a.mp4",
                              "ignore.txt", ".hidden.mp3", "A.flac"};
static int entry;
static void *open_dir(const char *p) {
  assert(!strcmp(p, "/sd/media"));
  entry = 0;
  return (void *)1;
}
static int read_dir(void *d, char *n, size_t cap, int *isdir) {
  (void)d;
  if (entry == 6)
    return 0;
  *isdir = entry == 1;
  snprintf(n, cap, "%s", names[entry++]);
  return 1;
}
static int close_dir(void *d) {
  assert(d == (void *)1);
  return 0;
}
static int resolve(const char *s, uint32_t *ip) {
  assert(!strcmp(s, "127.0.0.1"));
  *ip = 1;
  return 1;
}
static int connect_tcp(uint32_t ip, uint16_t port) {
  assert(ip == 1 && port == 5001);
  return 7;
}
static int poll_fd(int fd) {
  assert(fd == 7);
  return 3;
}
static int send_fd(int fd, const void *p, int n) {
  assert(fd == 7);
  if (n > 7)
    n = 7;
  memcpy(request + sent, p, n);
  sent += n;
  request[sent] = 0;
  return n;
}
static int recv_fd(int fd, void *p, int n) {
  assert(fd == 7);
  size_t left = strlen(response) - received;
  if (!left)
    return 0;
  if (n > 3)
    n = 3;
  if ((size_t)n > left)
    n = (int)left;
  memcpy(p, response + received, n);
  received += n;
  return n;
}
static void close_fd(int fd) {
  assert(fd == 7);
  closed++;
}
static int64_t now(void) { return clock_us; }
static void delay(int ms) { clock_us += ms * 1000; }
static int cancel(void) { return 1; }
static void response_body(const char *body) {
  snprintf(response, sizeof response,
           "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n%s", strlen(body),
           body);
  sent = received = 0;
  clock_us = 0;
}
static int packed_dir(const char *p, char *out, int cap) {
  assert(!strcmp(p, "/sd/media"));
  const char data[] =
      "z.MP3\0folder/\0a.mp4\0ignore.txt\0.hidden.mp3\0A.flac\0";
  assert(cap >= (int)sizeof data);
  memcpy(out, data, sizeof data);
  return 6;
}
int main(void) {
  const char *good =
      "{\"ok\":true,\"title\":\"M\\u00e9dia "
      "\\ud83d\\ude00\",\"total\":34,\"offset\":32,\"items\":[{\"title\":"
      "\"One\",\"id\":\"media-source://one\",\"type\":\"audio/"
      "mpeg\",\"class\":\"music\",\"dir\":false,\"play\":true},{\"title\":"
      "\"Folder\",\"id\":\"folder\",\"dir\":true,\"play\":false}],\"future\":{"
      "\"nested\":[true,null,5]}}";
  assert(media_parse(good, strlen(good), &listing, error) == 0);
  assert(listing.count == 2 && listing.offset == 32 && listing.total == 34 &&
         listing.items[1].directory && media_kind(&listing.items[0]) == 1);
  assert(!strcmp(listing.title, "Média 😀"));
  const char *bad[] = {
      "{}", "{\"ok\":true}",
      "{\"ok\":true,\"title\":\"x\",\"total\":0,\"offset\":1,\"items\":[]}",
      "{\"ok\":true,\"title\":\"\\ud800\",\"total\":0,\"offset\":0,\"items\":[]"
      "}",
      "{\"ok\":true,\"title\":\"\\u0000\",\"total\":0,\"offset\":0,\"items\":[]"
      "}"};
  for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++)
    assert(media_parse(bad[i], strlen(bad[i]), &listing, error) < 0);
  for (size_t n = 0; n < strlen(good); n++)
    assert(media_parse(good, n, &listing, error) < 0);
  assert(media_parse("{\"ok\":true}", 11, NULL, error) == 0);
  const char *failed = "{\"ok\":false,\"message\":\"HA denied\"}";
  assert(media_parse(failed, strlen(failed), NULL, error) < 0 &&
         !strcmp(error, "HA denied"));
  app_services_t s = {0};
  s.mem_alloc = malloc;
  s.mem_free = free;
  s.dir_open = open_dir;
  s.dir_read = read_dir;
  s.dir_close = close_dir;
  assert(media_sd_list(&s, "/sd/media", 0, &listing, error, NULL) == 0);
  assert(listing.count == 4 && listing.items[0].directory &&
         !strcmp(listing.items[1].title, "A.flac"));
  assert(!strcmp(listing.items[3].id, "/sd/media/z.MP3"));
  assert(media_sd_list(&s, "/sd/roms", 0, &listing, error, NULL) < 0);
  assert(media_sd_list(&s, "/sd/media", 5, &listing, error, NULL) < 0);
  s.dir_open = 0;
  s.dir_read = 0;
  s.dir_close = 0;
  s.file_list_dir = packed_dir;
  assert(media_sd_list(&s, "/sd/media", 0, &listing, error, NULL) == 0);
  assert(listing.count == 4 && listing.items[0].directory);
  assert(!strcmp(listing.items[3].id, "/sd/media/z.MP3"));
  s.net_resolve = resolve;
  s.net_tcp_connect = connect_tcp;
  s.net_poll = poll_fd;
  s.net_tcp_send = send_fd;
  s.net_tcp_recv = recv_fd;
  s.net_udp_close = close_fd;
  s.get_time_us = now;
  s.delay_ms = delay;
  response_body(good);
  assert(media_browse(&s, "quote\"\\\n", 32, &listing, error, NULL) == 0);
  assert(strstr(request, "POST /browse ") &&
         strstr(request, "quote\\\"\\\\\\u000a") &&
         strstr(request, "\"offset\":32"));
  assert(closed == 1);
  response_body("{\"ok\":true}");
  assert(media_control(&s, "pause", error, NULL) == 0 &&
         strstr(request, "\"command\":\"pause\""));
  response_body("{\"ok\":true}");
  assert(media_control(&s, "stop", error, cancel) < 0 &&
         strstr(error, "cancelled"));
  assert(closed == 3);
  response_body(good);
  strcpy(response,
         "HTTP/1.1 200 OK\r\nContent-Length: 12evil\r\n\r\n{\"ok\":true}");
  assert(media_control(&s, "stop", error, NULL) < 0);
  puts("Media parser: Unicode, truncated/malformed replies, page bounds, SD "
       "filtering/sort, fragmented HTTP, escaping and cancellation passed");
}
