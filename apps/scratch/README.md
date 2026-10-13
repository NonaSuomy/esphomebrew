# Scratch Everywhere! for PAX

An ESP32-P4 PAX block editor and player for Scratch 3 `.sb3` projects, based on the pinned ScratchEverywhere runtime. Create projects directly with touch or a USB mouse, or open exported projects from Scratch on your computer.

## Install

1. Put `scratch.pax` on SD at `/sd/roms/pax/scratch.pax`, or launch it from the PAX store.
2. Extract `scratch-data.zip` into `/sd/roms/scratch/`. The result must contain
   `romfs/gfx/translations/languages.json`, `projects/`, and `settings/`.
3. Add `.sb3` files to `/sd/roms/scratch/projects/`. An original `PAX Demo.sb3`
   tests SVG/PNG costumes, scripts, a variable monitor, pen trails, speech and sound.
4. Launch Scratch. Touch **New** to create a project, or **Edit** followed by an existing project to open its scripts. Leave Edit off to play a project with touch or Up/Down and A/Enter.

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


## Block editor (0.2.0)

The editor uses an 800×480 surface fitted to the PAX canvas, while playback keeps the existing 480×360 surface. Choose a category in the left column, then drag or tap a palette block to add it. Drag a stack under another block, inside a loop/condition, or drag a round/green reporter into an input. A red line marks a compatible drop target. Dragging a middle block carries the blocks below it and any nested scripts.

Tap an input to change its value using the on-screen keyboard or USB keyboard. Tap a connected reporter to edit its own inputs. **Shift** supplies capital letters and symbols. Select sprites on the right; arrows page through larger projects. **Sprite +** adds a simple sprite with a built-in beep sound. **Var +** creates a sprite variable. Existing project costumes and sounds can be selected by their names in the relevant block inputs.

**Run** exports a temporary snapshot and opens it in the player. Touch **STOP** in its top-right corner, press Escape, or use Select+B to return to editing. The stage pane then shows the last run. **Save** creates a new `.sb3` file and preserves the original; choose a different name for another save. The matching `.sb3.json` keeps imported controls and streams archives from SD to reduce memory use.

**Copy** duplicates a selected stack. **Trash**, or dropping a stack on the palette, removes it. **Undo/Redo** restores edits (up to 20 snapshots within a 1 MiB history budget). **Pan** lets you drag the workspace, mouse-wheel scrolls it, and **Home** brings scripts back into view. Back/New confirm unsaved edits. Holding Menu to exit saves an unsaved workspace as `Recovered-<timestamp>.sb3` when storage permits.

This is a native first version with Motion, Looks, Sound, Events, Control, Sensing, Operators, Variables and Pen blocks. It supports stacks, nested repeat/forever/if/if-else, round/boolean inputs, sprite selection and exported projects. Costume/sound drawing or recording, list editing, custom procedures, cloud/Lua extensions and full Scratch GUI parity are outside this version. Unknown imported blocks retain their JSON and assets and are shown by opcode. The initial stage thumbnail is approximate; Run uses the real interpreter and renderer.

## Source and builds

`core` links to the independently versioned pinned checkout in `upstream/`.
`port/` owns the PAX adapters. `pax_editor_model.cpp` owns block graphs, history and SB3 export; `pax_editor_ui.cpp` owns the touch interface. `deps/` keeps exact dependency snapshots and their
licenses; see `provenance.json`. The Plutovg patch replaces `unsigned int*` with
`uint32_t*` in its fill/gradient buffers because ESP newlib uses `unsigned long`
for `uint32_t`. The PAX ABI and firmware are unchanged.

From the PAX workspace, `./pax-recipe/build-modules.sh scratch` builds the package.
`python3 apps/scratch/build.py --host` builds the native smoke test; run
`python3 apps/scratch/tests/make_demo.py` before `apps/scratch/build-host/scratch-test`.
The standalone builder also works after sourcing `pax-tools/env.sh`.

Compilation and host tests do not establish on-device performance or behavior.
