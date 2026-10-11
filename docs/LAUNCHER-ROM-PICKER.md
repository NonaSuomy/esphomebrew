# Emulator ROM selection

Open a ROM-based emulator from the launcher, or choose Launch in its app details. The game selector lists compatible files from `roms/<appname>/` on SD and USB. Up/Down selects a game, A launches it, and B returns without changing the selection. Touch a game row to launch it. Empty folders still allow Back.

NES Plus scans both `roms/nesplus/` and `roms/nes/`. Supported extensions match the active emulator adapters. ZIPs are extracted to an emulator-specific cache before launch; Neo Geo ZIPs are passed intact because the emulator needs the archive. BIOS and other required support files remain necessary; see each app's installation recipe.

The selected path is saved as `roms/pax/<appname>.rom` on SD and set in memory for the launch. Normal and `-debug` packages share the same per-emulator selection. Up to 128 games are listed; complete paths must fit the 255-byte launcher limit. Long display names are shortened while the complete launch path is preserved. Keep games directly in the listed folders.
