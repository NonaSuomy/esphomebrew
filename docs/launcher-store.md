# Launcher UI store release

`apps/launcher_ui/pax.json` makes Launcher UI a regular published store app.
The LVGL dependency remains pinned; the application sources come from
`local:apps/launcher_ui` in the store checkout. The explicit `files` list
avoids compiling Font Awesome glyphs twice because `main.c` includes them.
The shared configuration/runtime live in `apps/lvgl`, and the linker script
is `apps/_shared/psram_app.ld`.

`sdk_headers` stages only the ABI and protocol headers named in the manifest.
It uses the authoritative sibling `pax-esphome` SDK in the workspace and the
mirrored `esphome/components/pax_loader` headers in a standalone checkout.
Other apps keep their existing SDK build behavior.

Build with the ESP-IDF RISC-V toolchain on PATH:

```sh
python3 tools/build_pax.py launcher_ui --out dist
```

This creates `launcher_ui.pax` and its generated `launcher_ui.json` listing.
The store publisher produces the versioned download and JSON sidecar, for
example `launcher_ui-0.6.1.pax` and `launcher_ui-0.6.1.json`. Bump the manifest
version whenever changing a published binary. The SD starter installer keeps
its separately pinned release until deliberately refreshed.
