# Scratch Everywhere! for PAX

An ESP32-P4 PAX block editor and player for Scratch 3 `.sb3` projects, based on the pinned ScratchEverywhere runtime. Create projects directly with touch or a USB mouse, or open exported projects from Scratch on your computer.

## Install

1. Put `scratch.pax` on SD at `/sd/roms/pax/scratch.pax`, or launch it from the PAX store.
2. Extract `scratch-data.zip` into `/sd/roms/scratch/`. The result must contain
   `romfs/gfx/translations/languages.json`, `projects/`, and `settings/`.
3. Add `.sb3` files to `/sd/roms/scratch/projects/`. An original `PAX Demo.sb3`
   tests SVG/PNG costumes, scripts, a variable monitor, pen trails, speech and sound.
4. Launch Scratch. Touch **New** to create a project, or a project filename to open its scripts, or **Edit** to open the selected project directly. Use Up/Down and A/Enter to play a project.

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
Player text overlays use a small ASCII font; the editor uses antialiased Noto Sans from the existing asset pack. SVG costume text uses the bundled fonts.
Date/time sensing depends on the loader's available clock and currently uses
uptime. Complex Scratch projects may exceed the device's memory/CPU budget.


## Block editor (0.3.0)

The editor negotiates the PAX canvas and uses all 1024×600 pixels on Elecrow (800×480 fallback for older loaders). Extra width expands the workspace, and the stage, toolbar and footer stay anchored. Playback keeps its existing 480×360 stage surface. Choose a category in the left column, then drag or tap a palette block to add it. Palette items size themselves to their contents, wrap long labels, and leave clear space around hat tops, tabs and C shapes. Inline script elements have explicit gaps and matching text measurements. Drag a stack under another block, inside a loop/condition, or drag a round/green reporter into an input. A red line marks a compatible drop target. Dragging a middle block carries the blocks below it and any nested scripts.

Tap an input to change its value using the on-screen keyboard or USB keyboard. Connected reporters are drawn inline; tap their own sockets to edit their inputs. **Shift** supplies capital letters and symbols. Select sprites on the right; arrows page through larger projects. **Sprite +** adds a simple sprite with a built-in beep sound. **Var +** creates a sprite variable. Existing project costumes and sounds can be selected by their names in the relevant block inputs.

**Run** exports a temporary snapshot and opens it in the player. Touch **STOP** in its top-right corner, press Escape, or use Select+B to return to editing. The stage pane then shows the last run. **Save** creates a new `.sb3` file and preserves the original; choose a different name for another save. The matching `.sb3.json` keeps imported controls and streams archives from SD to reduce memory use.

**Copy** duplicates a selected stack. **Trash**, or dropping a stack on the palette, removes it. **Undo/Redo** restores edits (up to 20 snapshots within a 1 MiB history budget). **Pan** lets you drag the workspace, mouse-wheel scrolls it, and **Home** brings scripts back into view. Back/New confirm unsaved edits. Open preserves dirty work in a recovery draft before replacing the workspace. Load errors remain visible and keep New/Open usable. Holding Menu to exit saves an unsaved workspace as `Recovered-<timestamp>.sb3` when storage permits.

This is a native editor with Motion, Looks, Sound, Events, Control, Sensing, Operators, Variables, Pen and Board IO blocks. It supports stacks, nested repeat/forever/if/if-else, round/boolean inputs, sprite selection and exported projects. Costume/sound drawing or recording, list editing, custom procedures, cloud/Lua extensions and full Scratch GUI parity are outside this version. Unknown imported blocks retain their JSON and assets and are shown by opcode. The initial stage thumbnail is approximate; Run uses the real interpreter and renderer.

## Board I/O

See the firmware workspace documentation in `pax-esphome/docs/scratch-hardware.md` for channels, units, ranges, profiles and pin conflicts. Board IO reads configured ESPHome sensors and queues actuator commands. Sensor values are not simulated when firmware support or a sample is missing. The camera uses an optional experimental profile.

## Source and builds

`core` links to the independently versioned pinned ScratchEverywhere checkout in `upstream/`. The official browser-editor reference is kept separately in `upstream/scratch-editor-reference`, pinned in `provenance.json`; it is not part of the native binary.
`port/` owns the PAX adapters. `pax_editor_model.cpp` owns block graphs, history and SB3 export; `pax_editor_ui.cpp` owns the touch interface. `deps/` keeps exact dependency snapshots and their
licenses; see `provenance.json`. The Plutovg patch replaces `unsigned int*` with
`uint32_t*` in its fill/gradient buffers because ESP newlib uses `unsigned long`
for `uint32_t`. The PAX ABI remains version 1. The optional I/O bridge appends four service pointers; hardware blocks require the updated firmware.

From the PAX workspace, `./pax-recipe/build-modules.sh scratch` builds the package.
`python3 apps/scratch/build.py --host` builds the native smoke test; run
`python3 apps/scratch/tests/make_demo.py` before `apps/scratch/build-host/scratch-test`.
The standalone builder also works after sourcing `pax-tools/env.sh`.

Compilation and host tests do not establish on-device performance or behavior.

Toolbar artwork: the supplied Scratch green flag and red stop SVGs are embedded in `port/pax_icons.h`. New/Open/Save, Undo/Redo, Home, Pan/Blocks, and Back use matching vector glyphs. The controls retain their original touch hit areas; icons require no additional SD files.

Desktop input: move/click the mouse to select projects or manipulate blocks. In the project picker, arrows change the selection, Enter previews, E opens the selected project in the editor, and N creates a project. Short key taps are handled even when press/release arrive in the same input poll.

The Scratch project timer uses the execution-step timestamp, matching scratch-vm `io/clock.js`. Reset followed by a timer read within a step is exactly zero. Wait/audio/frame timers continue using real time.
