#pragma once

#include <stdint.h>

// Stratagus includes its LAN headers from several otherwise single-player
// translation units.  PAX deliberately has no socket backend; these helpers
// keep byte-order code in those units compilable while the network objects are
// omitted from the image.
static inline uint16_t pax_bswap16(uint16_t v)
{
	return (uint16_t)((v << 8) | (v >> 8));
}

static inline uint32_t pax_bswap32(uint32_t v)
{
	return ((v & 0x000000ffu) << 24) | ((v & 0x0000ff00u) << 8) |
		((v & 0x00ff0000u) >> 8) | ((v & 0xff000000u) >> 24);
}

#ifndef htons
#define htons(v) pax_bswap16((uint16_t)(v))
#define ntohs(v) pax_bswap16((uint16_t)(v))
#define htonl(v) pax_bswap32((uint32_t)(v))
#define ntohl(v) pax_bswap32((uint32_t)(v))
#endif
