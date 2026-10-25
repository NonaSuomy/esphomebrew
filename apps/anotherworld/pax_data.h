#pragma once
#include <stddef.h>
#include <stdint.h>

extern unsigned char gb_memlist_bin[3000];
extern unsigned int gb_memlist_bin_size;
bool pax_aw_load_memlist();
bool pax_aw_read_bank(unsigned bank, uint32_t offset, void *buffer, size_t bytes);
bool pax_aw_validate_memlist(const unsigned char *data, size_t bytes);
