#pragma once

// Small libc compatibility layer for Stratagus's freestanding PAX build.
// Keep this header limited to APIs used by the engine's portable code; SDL
// and the filesystem wrappers remain in pax_sdl_bridge.cpp/pax_libc.c.

#include <ctype.h>
#include <stddef.h>

static inline double pax_strtod(const char *s, char **end)
{
	const char *p = s; int sign = 1;
	while (*p == ' ' || *p == '\t') ++p;
	if (*p == '-' || *p == '+') { if (*p++ == '-') sign = -1; }
	double v = 0.0;
	while (*p >= '0' && *p <= '9') v = v * 10.0 + (*p++ - '0');
	if (*p == '.') { double q = 0.1; ++p; while (*p >= '0' && *p <= '9') { v += (*p++ - '0') * q; q *= 0.1; } }
	if (end) *end = (char *)p;
	return sign < 0 ? -v : v;
}

static inline unsigned long pax_strtoul(const char *s, char **end, int base)
{
	const char *p = s;
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
	if (base == 0) {
		base = 10;
		if (p[0] == '0') {
			if (p[1] == 'x' || p[1] == 'X') { base = 16; p += 2; }
			else base = 8;
		}
	} else if (base == 16 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
		p += 2;
	}
	const char *first = p;
	unsigned long value = 0;
	for (;;) {
		int digit = (*p >= '0' && *p <= '9') ? *p - '0' :
			(*p >= 'a' && *p <= 'z') ? *p - 'a' + 10 :
			(*p >= 'A' && *p <= 'Z') ? *p - 'A' + 10 : -1;
		if (digit < 0 || digit >= base) break;
		value = value * (unsigned)base + (unsigned)digit;
		++p;
	}
	if (end) *end = (char *)(p == first ? s : p);
	return value;
}

static inline long pax_strtol(const char *s, char **end, int base)
{
	const char *p = s;
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
	int negative = 0;
	if (*p == '+' || *p == '-') { negative = (*p++ == '-'); }
	char *parsed = NULL;
	unsigned long value = pax_strtoul(p, &parsed, base);
	if (end) *end = (parsed == p) ? (char *)s : parsed;
	return negative ? -(long)value : (long)value;
}
#ifndef __cplusplus
#define strtod pax_strtod
#endif

static inline int pax_isascii(int c)
{
	return (unsigned)c < 128u;
}

static inline int pax_tolower(int c)
{
	return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static inline int pax_strncasecmp(const char *a, const char *b, size_t n)
{
	while (n != 0) {
		const unsigned char ca = (unsigned char)pax_tolower((unsigned char)*a++);
		const unsigned char cb = (unsigned char)pax_tolower((unsigned char)*b++);
		if (ca != cb || ca == 0) {
			return (int)ca - (int)cb;
		}
		--n;
	}
	return 0;
}

#ifndef isascii
#define isascii(c) pax_isascii(c)
#endif

#ifndef strncasecmp
#define strncasecmp(a, b, n) pax_strncasecmp((a), (b), (n))
#endif
