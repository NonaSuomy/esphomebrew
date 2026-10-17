#include "media_client.h"
#include <stdint.h>
static size_t size(const char *s) {
  size_t n = 0;
  while (s[n])
    ++n;
  return n;
}
static void clear(void *v, size_t n) {
  unsigned char *p = v;
  while (n--)
    *p++ = 0;
}
static void copy(char *d, const char *s, size_t n) {
  size_t i = 0;
  while (i + 1 < n && s[i]) {
    d[i] = s[i];
    ++i;
  }
  if (n)
    d[i] = 0;
}
static int same(const char *a, const char *b) {
  while (*a && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}
static int prefix(const char *a, const char *b) {
  while (*b)
    if (*a++ != *b++)
      return 0;
  return 1;
}
static int fail(char *error, const char *s) {
  copy(error, s, MEDIA_ERROR);
  return -1;
}
typedef struct {
  const char *p, *end;
  unsigned depth;
  int bad;
} json_t;
static void space(json_t *j) {
  while (j->p < j->end &&
         (*j->p == ' ' || *j->p == '\n' || *j->p == '\r' || *j->p == '\t'))
    ++j->p;
}
static int take(json_t *j, char c) {
  space(j);
  if (j->p == j->end || *j->p != c) {
    j->bad = 1;
    return 0;
  }
  ++j->p;
  return 1;
}
static int hex(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}
static unsigned u16(json_t *j) {
  unsigned x = 0;
  for (unsigned n = 0; n < 4; n++) {
    if (j->p == j->end || hex(*j->p) < 0) {
      j->bad = 1;
      return 0;
    }
    x = x * 16 + (unsigned)hex(*j->p++);
  }
  return x;
}
static void emit(json_t *j, char *out, size_t cap, size_t *used,
                 unsigned char c) {
  if (out) {
    if (*used + 1 >= cap)
      j->bad = 1;
    else
      out[*used] = (char)c;
  }
  ++*used;
}
static void string(json_t *j, char *out, size_t cap) {
  size_t n = 0;
  if (!take(j, '"'))
    return;
  while (j->p < j->end && !j->bad) {
    unsigned char c = (unsigned char)*j->p++;
    if (c == '"') {
      if (out)
        out[n < cap ? n : cap - 1] = 0;
      return;
    }
    if (c < 32) {
      j->bad = 1;
      break;
    }
    if (c != '\\') {
      emit(j, out, cap, &n, c);
      continue;
    }
    if (j->p == j->end) {
      j->bad = 1;
      break;
    }
    c = (unsigned char)*j->p++;
    if (c == '"' || c == '\\' || c == '/')
      emit(j, out, cap, &n, c);
    else if (c == 'b' || c == 'f' || c == 'n' || c == 'r' || c == 't')
      emit(j, out, cap, &n,
           c == 'b'   ? 8
           : c == 'f' ? 12
           : c == 'n' ? 10
           : c == 'r' ? 13
                      : 9);
    else if (c == 'u') {
      unsigned u = u16(j);
      if (u >= 0xd800 && u <= 0xdbff) {
        if (j->end - j->p < 2 || j->p[0] != '\\' || j->p[1] != 'u') {
          j->bad = 1;
          break;
        }
        j->p += 2;
        unsigned low = u16(j);
        if (low < 0xdc00 || low > 0xdfff) {
          j->bad = 1;
          break;
        }
        u = 0x10000 + ((u - 0xd800) << 10) + (low - 0xdc00);
      } else if (u >= 0xdc00 && u <= 0xdfff) {
        j->bad = 1;
        break;
      }
      if (!u) {
        j->bad = 1;
        break;
      }
      if (u < 0x80)
        emit(j, out, cap, &n, (unsigned char)u);
      else if (u < 0x800) {
        emit(j, out, cap, &n, 0xc0 | (u >> 6));
        emit(j, out, cap, &n, 0x80 | (u & 63));
      } else if (u < 0x10000) {
        emit(j, out, cap, &n, 0xe0 | (u >> 12));
        emit(j, out, cap, &n, 0x80 | ((u >> 6) & 63));
        emit(j, out, cap, &n, 0x80 | (u & 63));
      } else {
        emit(j, out, cap, &n, 0xf0 | (u >> 18));
        emit(j, out, cap, &n, 0x80 | ((u >> 12) & 63));
        emit(j, out, cap, &n, 0x80 | ((u >> 6) & 63));
        emit(j, out, cap, &n, 0x80 | (u & 63));
      }
    } else
      j->bad = 1;
  }
  j->bad = 1;
}
static int literal(json_t *j, const char *s) {
  while (*s)
    if (j->p == j->end || *j->p++ != *s++) {
      j->bad = 1;
      return 0;
    }
  return 1;
}
static void skip(json_t *j) {
  space(j);
  if (j->p == j->end || ++j->depth > 16) {
    j->bad = 1;
    return;
  }
  char c = *j->p;
  if (c == '"')
    string(j, 0, 0);
  else if (c == '{' || c == '[') {
    ++j->p;
    char end = c == '{' ? '}' : ']';
    space(j);
    if (j->p < j->end && *j->p == end)
      ++j->p;
    else
      for (;;) {
        if (c == '{') {
          string(j, 0, 0);
          take(j, ':');
        }
        skip(j);
        space(j);
        if (j->bad)
          break;
        if (j->p < j->end && *j->p == end) {
          ++j->p;
          break;
        }
        if (!take(j, ','))
          break;
      }
  } else if (c == 't')
    literal(j, "true");
  else if (c == 'f')
    literal(j, "false");
  else if (c == 'n')
    literal(j, "null");
  else {
    if (c == '-')
      ++j->p;
    if (j->p == j->end || *j->p < '0' || *j->p > '9')
      j->bad = 1;
    else {
      if (*j->p == '0')
        ++j->p;
      else
        while (j->p < j->end && *j->p >= '0' && *j->p <= '9')
          ++j->p;
      if (j->p < j->end && *j->p == '.') {
        ++j->p;
        const char *start = j->p;
        while (j->p < j->end && *j->p >= '0' && *j->p <= '9')
          ++j->p;
        if (start == j->p)
          j->bad = 1;
      }
      if (j->p < j->end && (*j->p == 'e' || *j->p == 'E')) {
        ++j->p;
        if (j->p < j->end && (*j->p == '+' || *j->p == '-'))
          ++j->p;
        const char *start = j->p;
        while (j->p < j->end && *j->p >= '0' && *j->p <= '9')
          ++j->p;
        if (start == j->p)
          j->bad = 1;
      }
    }
  }
  --j->depth;
}
static unsigned number(json_t *j) {
  space(j);
  unsigned n = 0;
  const char *start = j->p;
  while (j->p < j->end && *j->p >= '0' && *j->p <= '9') {
    if (n > 100000) {
      j->bad = 1;
      return 0;
    }
    n = n * 10 + (unsigned)(*j->p++ - '0');
  }
  if (start == j->p || n > 1000000)
    j->bad = 1;
  return n;
}
static int boolean(json_t *j) {
  space(j);
  if (j->p < j->end && *j->p == 't') {
    literal(j, "true");
    return 1;
  }
  literal(j, "false");
  return 0;
}
static void item(json_t *j, media_item_t *v) {
  if (!take(j, '{'))
    return;
  space(j);
  if (j->p < j->end && *j->p == '}') {
    ++j->p;
    return;
  }
  for (;;) {
    char key[64];
    string(j, key, sizeof key);
    if (!take(j, ':'))
      return;
    if (same(key, "title"))
      string(j, v->title, sizeof v->title);
    else if (same(key, "id"))
      string(j, v->id, sizeof v->id);
    else if (same(key, "type"))
      string(j, v->type, sizeof v->type);
    else if (same(key, "class"))
      string(j, v->category, sizeof v->category);
    else if (same(key, "dir"))
      v->directory = boolean(j);
    else if (same(key, "play"))
      v->playable = boolean(j);
    else
      skip(j);
    space(j);
    if (j->bad)
      return;
    if (j->p < j->end && *j->p == '}') {
      ++j->p;
      return;
    }
    if (!take(j, ','))
      return;
  }
}
int media_parse(const char *data, size_t n, media_listing_t *out, char *error) {
  json_t j = {data, data + n, 0, 0};
  int ok = -1;
  unsigned fields = 0;
  error[0] = 0;
  if (out)
    clear(out, sizeof *out);
  if (!take(&j, '{'))
    return fail(error, "Invalid media response");
  space(&j);
  if (j.p < j.end && *j.p == '}') {
    ++j.p;
    j.bad = 1;
  } else
    for (;;) {
      char key[64];
      string(&j, key, sizeof key);
      if (!take(&j, ':'))
        break;
      if (same(key, "ok"))
        ok = boolean(&j);
      else if (same(key, "message"))
        string(&j, error, MEDIA_ERROR);
      else if (out && same(key, "title")) {
        fields |= 1;
        string(&j, out->title, sizeof out->title);
      } else if (out && same(key, "total")) {
        fields |= 2;
        out->total = number(&j);
      } else if (out && same(key, "offset")) {
        fields |= 4;
        out->offset = number(&j);
      } else if (out && same(key, "items")) {
        fields |= 8;
        if (!take(&j, '['))
          break;
        space(&j);
        if (j.p < j.end && *j.p == ']')
          ++j.p;
        else
          for (;;) {
            if (out->count == MEDIA_PAGE) {
              j.bad = 1;
              break;
            }
            item(&j, &out->items[out->count++]);
            space(&j);
            if (j.bad)
              break;
            if (j.p < j.end && *j.p == ']') {
              ++j.p;
              break;
            }
            if (!take(&j, ','))
              break;
          }
      } else
        skip(&j);
      space(&j);
      if (j.bad)
        break;
      if (j.p < j.end && *j.p == '}') {
        ++j.p;
        break;
      }
      if (!take(&j, ','))
        break;
    }
  space(&j);
  if (j.bad || j.p != j.end || ok < 0)
    return fail(error, "Invalid or oversized media response");
  if (!ok)
    return error[0] ? -1 : fail(error, "Media request failed");
  if (out && fields != 15)
    return fail(error, "Incomplete media listing");
  if (out &&
      (out->offset > out->total || out->count > out->total - out->offset))
    return fail(error, "Invalid media page bounds");
  error[0] = 0;
  return 0;
}
typedef struct {
  char *data;
  size_t used, cap;
  int bad;
} text_t;
static void add(text_t *t, const char *s) {
  while (*s) {
    if (t->used + 1 >= t->cap) {
      t->bad = 1;
      return;
    }
    t->data[t->used++] = *s++;
  }
  t->data[t->used] = 0;
}
static void integer(text_t *t, unsigned n) {
  char s[12];
  unsigned i = 0;
  do {
    s[i++] = (char)('0' + n % 10);
    n /= 10;
  } while (n);
  while (i) {
    char x[2] = {s[--i], 0};
    add(t, x);
  }
}
static void quote(text_t *t, const char *s) {
  add(t, "\"");
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c < 32) {
      char x[7] = {'\\',
                   'u',
                   '0',
                   '0',
                   "0123456789abcdef"[c >> 4],
                   "0123456789abcdef"[c & 15],
                   0};
      add(t, x);
    } else {
      if (c == '"' || c == '\\')
        add(t, "\\");
      char x[2] = {(char)c, 0};
      add(t, x);
    }
  }
  add(t, "\"");
}
int media_rpc(const app_services_t *s, const char *path, const char *body,
              media_listing_t *out, char *error, int (*pump)(void)) {
  error[0] = 0;
  uint32_t ip = 0;
  int fd = -1, result = -1;
  char *request = 0, *response = 0;
  if (!s->net_resolve("127.0.0.1", &ip) ||
      (fd = s->net_tcp_connect(ip, 5001)) < 0)
    return fail(error, "Update firmware: media browser bridge is unavailable");
  request = s->mem_alloc(4608);
  response = s->mem_alloc(61025);
  if (!request || !response) {
    fail(error, "Not enough memory for media request");
    goto done;
  }
  text_t t = {request, 0, 4608, 0};
  add(&t, "POST ");
  add(&t, path);
  add(&t, " HTTP/1.1\r\nHost: 127.0.0.1:5001\r\nConnection: "
          "close\r\nContent-Type: application/json\r\nContent-Length: ");
  integer(&t, (unsigned)size(body));
  add(&t, "\r\n\r\n");
  add(&t, body);
  if (t.bad) {
    fail(error, "Media request is too long");
    goto done;
  }
  size_t sent = 0, used = 0, header = 0, wanted = 0;
  int status = 0, ready = 0;
  int64_t started = s->get_time_us();
  while (s->get_time_us() - started < 35000000) {
    if (pump && pump()) {
      fail(error, "Media request cancelled");
      goto done;
    }
    int flags = s->net_poll(fd);
    if (flags < 0 || (!ready && (flags & 4))) {
      fail(error, "Update firmware: media browser bridge is unavailable");
      goto done;
    }
    if (flags & 2)
      ready = 1;
    if (ready && sent < t.used) {
      int n = s->net_tcp_send(fd, request + sent, (int)(t.used - sent));
      if (n < 0) {
        fail(error, "Media request connection failed");
        goto done;
      }
      sent += (size_t)n;
    }
    if (sent == t.used) {
      int n = s->net_tcp_recv(fd, response + used, (int)(61024 - used));
      if (n > 0) {
        used += (size_t)n;
        response[used] = 0;
        if (!header) {
          for (size_t i = 0; i + 3 < used; i++)
            if (response[i] == '\r' && response[i + 1] == '\n' &&
                response[i + 2] == '\r' && response[i + 3] == '\n') {
              header = i + 4;
              break;
            }
          if (header) {
            if (header > 1024 || used < 12 || !prefix(response, "HTTP/1.1 ")) {
              fail(error, "Invalid media bridge HTTP response");
              goto done;
            }
            status = (response[9] - '0') * 100 + (response[10] - '0') * 10 +
                     response[11] - '0';
            const char *p = response;
            int have_length = 0;
            while (p < response + header) {
              const char *line = p;
              while (p < response + header && *p != '\n')
                ++p;
              if (prefix(line, "Content-Length: ")) {
                if (have_length++) {
                  fail(error, "Invalid media response length");
                  goto done;
                }
                json_t j = {line + 16, p, 0, 0};
                wanted = number(&j);
                space(&j);
                if (j.bad || j.p != j.end || wanted > 60000) {
                  fail(error, "Media listing is too large");
                  goto done;
                }
              }
              if (p < response + header)
                ++p;
            }
            if (!have_length) {
              fail(error, "Media response has no length");
              goto done;
            }
          } else if (used > 1024) {
            fail(error, "Media response header is too large");
            goto done;
          }
        }
        if (header && used >= header + wanted) {
          result = media_parse(response + header, wanted, out, error);
          if (status != 200 && result == 0)
            result = fail(error, "Home Assistant rejected the media request");
          goto done;
        }
        if (used == 61024) {
          fail(error, "Media response is too large");
          goto done;
        }
      } else if (n != -2) {
        fail(error, "Media response ended early");
        goto done;
      }
    }
    s->delay_ms(2);
  }
  fail(error, "Media request timed out; check Home Assistant connection and "
              "permission");
done:
  if (fd >= 0)
    s->net_udp_close(fd);
  if (request)
    s->mem_free(request);
  if (response)
    s->mem_free(response);
  return result;
}
int media_browse(const app_services_t *s, const char *id, unsigned offset,
                 media_listing_t *out, char *error, int (*pump)(void)) {
  char body[2304];
  text_t t = {body, 0, sizeof body, 0};
  add(&t, "{\"id\":");
  quote(&t, id);
  add(&t, ",\"offset\":");
  integer(&t, offset);
  add(&t, "}");
  if (t.bad)
    return fail(error, "Media folder id is too long");
  return media_rpc(s, "/browse", body, out, error, pump);
}
int media_play(const app_services_t *s, const media_item_t *item, int sd,
               char *error, int (*pump)(void)) {
  char body[4096];
  text_t t = {body, 0, sizeof body, 0};
  add(&t, "{\"id\":");
  quote(&t, item->id);
  add(&t, ",\"type\":");
  quote(&t, item->type);
  add(&t, ",\"sd\":");
  add(&t, sd ? "true}" : "false}");
  if (t.bad)
    return fail(error, "Media item id is too long");
  return media_rpc(s, "/play", body, 0, error, pump);
}
int media_control(const app_services_t *s, const char *command, char *error,
                  int (*pump)(void)) {
  char body[128];
  text_t t = {body, 0, sizeof body, 0};
  add(&t, "{\"command\":");
  quote(&t, command);
  add(&t, "}");
  if (t.bad)
    return fail(error, "Invalid media command");
  return media_rpc(s, "/control", body, 0, error, pump);
}
static int contains(const char *a, const char *b) {
  for (; *a; a++)
    if (prefix(a, b))
      return 1;
  return 0;
}
static int suffix(const char *a, const char *b) {
  size_t x = size(a), y = size(b);
  if (x < y)
    return 0;
  a += x - y;
  while (*a) {
    char c = *a++;
    if (c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
    if (c != *b++)
      return 0;
  }
  return 1;
}
int media_kind(const media_item_t *v) {
  if (prefix(v->type, "audio/") || contains(v->category, "music") ||
      same(v->type, "music") || suffix(v->id, ".mp3") ||
      suffix(v->id, ".flac") || suffix(v->id, ".wav"))
    return 1;
  if (prefix(v->type, "video/") || contains(v->category, "video") ||
      contains(v->category, "movie") || same(v->type, "video") ||
      suffix(v->id, ".mp4") || suffix(v->id, ".m4v") || suffix(v->id, ".avi") ||
      suffix(v->id, ".mjpg") || suffix(v->id, ".mjpeg"))
    return 2;
  return 0;
}
static int order(const char *a, const char *b) {
  while (*a && *b) {
    unsigned char x = (unsigned char)*a++, y = (unsigned char)*b++;
    if (x >= 'A' && x <= 'Z')
      x += 'a' - 'A';
    if (y >= 'A' && y <= 'Z')
      y += 'a' - 'A';
    if (x != y)
      return x < y ? -1 : 1;
  }
  return *a ? 1 : *b ? -1 : 0;
}
int media_sd_list(const app_services_t *s, const char *path, unsigned offset,
                  media_listing_t *out, char *error, int (*pump)(void)) {
  typedef struct {
    char name[256];
    int dir;
  } entry_t;
  if (!same(path, "/sd/media") && !prefix(path, "/sd/media/"))
    return fail(error, "SD browser stays inside /sd/media");
  if (contains(path, "..") || size(path) > 512)
    return fail(error, "Invalid SD media folder");
  const int streaming = s->dir_open && s->dir_read && s->dir_close;
  if (!streaming && !s->file_list_dir)
    return fail(error, "Update firmware: directory services are missing");
  if (s->file_mkdir)
    s->file_mkdir("/sd/media");
  void *dir = streaming ? s->dir_open(path) : 0;
  char *packed = streaming ? 0 : s->mem_alloc(131072);
  int packed_count = 0;
  size_t packed_at = 0;
  if ((!streaming && !packed) || (streaming && !dir))
    return fail(error, "Could not open /sd/media; check the SD card");
  if (!streaming) {
    packed_count = s->file_list_dir(path, packed, 131072);
    if (packed_count < 0 || packed_count > 256) {
      s->mem_free(packed);
      return fail(error,
                  packed_count < 0
                      ? "Could not read the SD folder"
                      : "SD folder has more than 256 entries; use subfolders");
    }
  }
  entry_t *entries = s->mem_alloc(256 * sizeof *entries);
  if (!entries) {
    if (streaming)
      s->dir_close(dir);
    if (packed)
      s->mem_free(packed);
    return fail(error, "Not enough memory to list SD media");
  }
  unsigned count = 0, seen = 0;
  int result = 0;
  for (;;) {
    char name[256];
    int is_dir = 0;
    int n;
    if (streaming)
      n = s->dir_read(dir, name, sizeof name, &is_dir);
    else if (!packed_count)
      n = 0;
    else {
      size_t start = packed_at;
      while (packed_at < 131072 && packed[packed_at])
        ++packed_at;
      size_t length = packed_at - start;
      if (packed_at == 131072 || !length)
        n = -1;
      else {
        is_dir = packed[packed_at - 1] == '/';
        if (is_dir)
          --length;
        if (length >= sizeof name)
          n = -1;
        else {
          for (size_t i = 0; i < length; i++)
            name[i] = packed[start + i];
          name[length] = 0;
          n = 1;
        }
        ++packed_at;
        --packed_count;
      }
    }
    if (n < 0) {
      result = fail(error, "Could not read the SD folder");
      break;
    }
    if (!n)
      break;
    if ((++seen & 7) == 0 && pump && pump()) {
      result = fail(error, "Media request cancelled");
      break;
    }
    if (name[0] == '.' || contains(name, "/") || contains(name, "\\"))
      continue;
    media_item_t file;
    clear(&file, sizeof file);
    copy(file.id, name, sizeof file.id);
    if (!is_dir && !media_kind(&file))
      continue;
    if (count == 256) {
      result = fail(error,
                    "SD folder has more than 256 media items; use subfolders");
      break;
    }
    unsigned at = count;
    while (at && ((is_dir && !entries[at - 1].dir) ||
                  (is_dir == entries[at - 1].dir &&
                   order(name, entries[at - 1].name) < 0))) {
      entries[at] = entries[at - 1];
      --at;
    }
    copy(entries[at].name, name, sizeof entries[at].name);
    entries[at].dir = is_dir;
    ++count;
  }
  if (streaming)
    s->dir_close(dir);
  if (packed)
    s->mem_free(packed);
  if (!result) {
    clear(out, sizeof *out);
    out->total = count;
    out->offset = offset;
    copy(out->title, path, sizeof out->title);
    if (offset > count)
      result = fail(error, "SD folder changed; reload the first page");
    else
      for (unsigned i = offset; i < count && out->count < MEDIA_PAGE; i++) {
        media_item_t *v = &out->items[out->count++];
        copy(v->title, entries[i].name, sizeof v->title);
        text_t t = {v->id, 0, sizeof v->id, 0};
        add(&t, path);
        add(&t, "/");
        add(&t, entries[i].name);
        if (t.bad) {
          result = fail(error, "SD media path is too long");
          break;
        }
        v->directory = entries[i].dir;
        v->playable = !v->directory;
        int kind = media_kind(v);
        copy(v->type,
             kind == 1
                 ? (suffix(v->id, ".mp3")    ? "audio/mpeg"
                    : suffix(v->id, ".flac") ? "audio/flac"
                                             : "audio/wav")
                 : (suffix(v->id, ".mp4") || suffix(v->id, ".m4v") ? "video/mp4"
                    : suffix(v->id, ".avi") ? "video/x-msvideo"
                                            : "video/x-motion-jpeg"),
             sizeof v->type);
      }
  }
  s->mem_free(entries);
  return result;
}
