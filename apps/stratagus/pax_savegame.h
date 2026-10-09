#pragma once
// Fixed SD slots backed by the engine's native save serialization.
int pax_stratagus_save_slot_exists(int slot);
int pax_stratagus_save_slot(int slot);
int pax_stratagus_request_load_slot(int slot);
const char *pax_stratagus_take_load_path();
void pax_stratagus_save_reload_completed();
void pax_stratagus_save_selftest();
// Named desktop-style saves remain confined to this game's SD directory.
int pax_stratagus_list_saves(char names[][64],int capacity);
int pax_stratagus_save_named(const char *name);
int pax_stratagus_load_named(const char *name);
int pax_stratagus_save_named_exists(const char *name);
int pax_stratagus_delete_named(const char *name);
