#include "relay_http.h"
#include <stddef.h>
#include <stdint.h>

typedef struct {
  char *data;
  size_t length, capacity;
  int failed;
} text_t;
static void add(text_t *b, const char *s) {
  while (*s) {
    if (b->length + 1 >= b->capacity) {
      b->failed = 1;
      return;
    }
    b->data[b->length++] = *s++;
  }
  b->data[b->length] = 0;
}
static void number(text_t *b, unsigned value) {
  char digits[12];
  unsigned n = 0;
  do {
    digits[n++] = (char)('0' + value % 10);
    value /= 10;
  } while (value);
  while (n) {
    char s[2] = {digits[--n], 0};
    add(b, s);
  }
}
static void quote(text_t *b, const char *s) {
  add(b, "\"");
  while (*s) {
    unsigned char c = (unsigned char)*s++;
    if (c < 32) {
      b->failed = 1;
      return;
    }
    if (c == '"' || c == '\\')
      add(b, "\\");
    char one[2] = {(char)c, 0};
    add(b, one);
  }
  add(b, "\"");
}
static int prefix(const char *s, const char *p) {
  while (*p)
    if (*p++ != *s++)
      return 0;
  return 1;
}
const char *relay_start_vnc(const app_services_t *svc, const char *computer,
                            unsigned computer_port, const char *password,
                            int (*pump)(void)) {
  char configuration[1400], ha_host[128];
  void *file = svc->file_open("/sd/roms/desktop/relay.conf", "rb");
  if (!file)
    return "Create /sd/roms/desktop/relay.conf: Home Assistant URL, then token";
  size_t n = svc->file_read(configuration, 1, sizeof configuration - 1, file);
  svc->file_close(file);
  configuration[n] = 0;
  char *token = configuration;
  while (*token && *token != '\n' && *token != '\r')
    token++;
  if (!*token)
    return "relay.conf needs two lines: Home Assistant URL and access token";
  *token++ = 0;
  while (*token == '\r' || *token == '\n')
    token++;
  char *end = token;
  while (*end && *end != '\r' && *end != '\n')
    end++;
  *end = 0;
  unsigned token_len = 0;
  for (char *p = token; *p; p++) {
    if ((unsigned char)*p < 33 || (unsigned char)*p > 126)
      return "Invalid relay API access token";
    token_len++;
  }
  if (!token_len || token_len > 1024)
    return "relay.conf access token is empty or too long";
  int tls = prefix(configuration, "https://");
  if (!tls && !prefix(configuration, "http://"))
    return "Use http:// or https:// in relay.conf";
  char *p = configuration + (tls ? 8 : 7);
  unsigned host_len = 0;
  while (*p && *p != ':' && *p != '/') {
    unsigned char c = (unsigned char)*p++;
    if (host_len + 1 >= sizeof ha_host ||
        !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '.' || c == '-'))
      return "Invalid Home Assistant hostname in relay.conf";
    ha_host[host_len++] = (char)c;
  }
  ha_host[host_len] = 0;
  if (!host_len)
    return "Home Assistant hostname is empty";
  unsigned ha_port = tls ? 443 : 80;
  if (*p == ':') {
    p++;
    ha_port = 0;
    unsigned digits = 0;
    while (*p >= '0' && *p <= '9') {
      if (++digits > 5)
        return "Invalid Home Assistant port";
      ha_port = ha_port * 10 + (unsigned)(*p++ - '0');
    }
    if (!digits || !ha_port || ha_port > 65535)
      return "Invalid Home Assistant port";
  }
  if (*p && !(p[0] == '/' && !p[1]))
    return "relay.conf URL must be an origin without a path";
  if (tls && (!svc->net_tls_connect || !svc->net_tls_status ||
              !svc->net_tls_send || !svc->net_tls_recv || !svc->net_tls_close))
    return "PAX firmware needs TLS services for this HTTPS URL";
  char body_data[768];
  text_t body = {body_data, 0, sizeof body_data, 0};
  add(&body, "{\"port\":5000,\"action\":\"start\",\"kind\":\"vnc\",\"host\":");
  quote(&body, computer);
  add(&body, ",\"server_port\":");
  number(&body, computer_port);
  add(&body, ",\"password\":");
  quote(&body, password);
  add(&body, "}");
  if (body.failed)
    return "VNC connection fields are too long";
  char *request = svc->mem_alloc(2304);
  if (!request)
    return "Not enough memory for relay request";
  text_t out = {request, 0, 2304, 0};
  add(&out, "POST /api/svp_relay/desktop HTTP/1.1\r\nHost: ");
  add(&out, ha_host);
  add(&out, ":");
  number(&out, ha_port);
  add(&out, "\r\nAuthorization: Bearer ");
  add(&out, token);
  add(&out, "\r\nContent-Type: application/json\r\nConnection: "
            "close\r\nContent-Length: ");
  number(&out, (unsigned)body.length);
  add(&out, "\r\n\r\n");
  add(&out, body_data);
  if (out.failed) {
    svc->mem_free(request);
    return "Relay API request too long";
  }
  int handle = -1, ready = 0;
  const char *error = "Could not connect to Home Assistant";
  if (tls)
    handle = svc->net_tls_connect(ha_host, (uint16_t)ha_port);
  else {
    uint32_t ip = 0;
    if (svc->net_resolve(ha_host, &ip))
      handle = svc->net_tcp_connect(ip, (uint16_t)ha_port);
  }
  int64_t started = svc->get_time_us();
  while (handle >= 0 && svc->get_time_us() - started < 30000000) {
    if (pump()) {
      error = "Relay request cancelled";
      break;
    }
    int state = tls ? svc->net_tls_status(handle) : svc->net_poll(handle);
    if (state < 0 || (!tls && (state & 4)))
      break;
    if (tls ? state == 1 : (state & 2)) {
      ready = 1;
      break;
    }
    svc->delay_ms(2);
  }
  if (ready) {
    size_t offset = 0;
    char line[64];
    unsigned line_used = 0;
    error = "Home Assistant relay request timed out";
    while (svc->get_time_us() - started < 45000000) {
      if (pump()) {
        error = "Relay request cancelled";
        break;
      }
      if (offset < out.length) {
        int sent = tls ? svc->net_tls_send(handle, request + offset,
                                           (int)(out.length - offset))
                       : svc->net_tcp_send(handle, request + offset,
                                           (int)(out.length - offset));
        if (sent < 0) {
          error = "Relay request connection failed";
          break;
        }
        if (sent > 0)
          offset += (size_t)sent;
      } else {
        char c;
        int got = tls ? svc->net_tls_recv(handle, &c, 1)
                      : svc->net_tcp_recv(handle, &c, 1);
        if (got > 0) {
          if (line_used + 1 >= sizeof line) {
            error = "Invalid HTTP response from Home Assistant";
            break;
          }
          line[line_used++] = c;
          line[line_used] = 0;
          if (c == '\n') {
            if (prefix(line, "HTTP/1.1 200 ") || prefix(line, "HTTP/1.0 200 "))
              error = 0;
            else if (prefix(line, "HTTP/1.1 401 ") ||
                     prefix(line, "HTTP/1.0 401 "))
              error = "Home Assistant rejected the access token";
            else if (prefix(line, "HTTP/1.1 404 ") ||
                     prefix(line, "HTTP/1.0 404 "))
              error = "Register this panel and port 5000 in SVP Relay first";
            else if (prefix(line, "HTTP/1.1 502 ") ||
                     prefix(line, "HTTP/1.0 502 "))
              error = "SVP Relay could not reach the VNC server or panel";
            else
              error = "Home Assistant rejected the relay desktop request";
            break;
          }
        } else if ((tls && got < 0) || (!tls && got != -2)) {
          error = "Home Assistant closed the request";
          break;
        }
      }
      svc->delay_ms(2);
    }
  }
  if (handle >= 0) {
    if (tls)
      svc->net_tls_close(handle);
    else
      svc->net_udp_close(handle);
  }
  volatile char *wipe = request;
  for (size_t i = 0; i < 2304; i++)
    wipe[i] = 0;
  svc->mem_free(request);
  wipe = configuration;
  for (size_t i = 0; i < sizeof configuration; i++)
    wipe[i] = 0;
  wipe = body_data;
  for (size_t i = 0; i < sizeof body_data; i++)
    wipe[i] = 0;
  return error;
}
