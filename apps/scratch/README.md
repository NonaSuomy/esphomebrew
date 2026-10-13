# Scratch Everywhere! for PAX

An ESP32-P4 PAX player for Scratch 3 `.sb3` projects, based on the pinned
ScratchEverywhere runtime. This runs projects; the Scratch block editor remains
on your computer. Export a project from Scratch using **File → Save to your computer**.

## Install

1. Put `scratch.pax` on SD at `/sd/roms/pax/scratch.pax`, or launch it from the PAX store.
2. Extract `scratch-data.zip` into `/sd/roms/scratch/`. The result must contain
   `romfs/gfx/translations/languages.json`, `projects/`, and `settings/`.
3. Add `.sb3` files to `/sd/roms/scratch/projects/`. An original `PAX Demo.sb3`
   tests SVG/PNG costumes, scripts, a variable monitor, pen trails, speech and sound.
4. Launch Scratch and touch a project, or choose with Up/Down and A/Enter.

Default project controls are D-pad = arrows and A = Space; B/X/Y/L/R retain
ScratchEverywhere's letter mappings. USB keyboard, USB mouse and touch work as
Scratch input. Hold Menu (or Start+Select) for two seconds to return to PAX.
Select+B stops the current project and opens the project browser. Ask-and-wait
uses a USB keyboard: type an answer, Backspace edits it, Enter submits.

Per-project `name.sb3.json` settings and controller mappings use the upstream
format. Recommended for larger projects: `{"settings":{"sb3InRam":false}}`.
The renderer uses a 480×360 RGB565 surface, scaled with letterboxing to the PAX
canvas. Standard sprite rotation, size, flip, ghost, brightness and pen are
implemented. Cloud variables, network downloads, custom Lua/native extensions,
advanced color/fisheye/whirl/pixelate/mosaic effects and clipboard are unavailable.
Text overlays use a small ASCII font; SVG costume text uses the bundled fonts.
Date/time sensing depends on the loader's available clock and currently uses
uptime. Complex Scratch projects may exceed the device's memory/CPU budget.

## Source and builds

`core` links to the independently versioned pinned checkout in `upstream/`.
`port/` owns the PAX adapters. `deps/` keeps exact dependency snapshots and their
licenses; see `provenance.json`. The Plutovg patch replaces `unsigned int*` with
`uint32_t*` in its fill/gradient buffers because ESP newlib uses `unsigned long`
for `uint32_t`. The PAX ABI and firmware are unchanged.

From the PAX workspace, `./pax-recipe/build-modules.sh scratch` builds the package.
`python3 apps/scratch/build.py --host` builds the native smoke test; run
`python3 apps/scratch/tests/make_demo.py` before `apps/scratch/build-host/scratch-test`.
The standalone builder also works after sourcing `pax-tools/env.sh`.

Compilation and host tests do not establish on-device performance or behavior.
