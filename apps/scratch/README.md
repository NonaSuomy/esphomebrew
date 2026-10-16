# Scratch Everywhere! for PAX

An ESP32-P4 PAX block editor and player for Scratch 3 `.sb3` projects, based on the pinned ScratchEverywhere runtime. Create projects directly with touch or a USB mouse, or open exported projects from Scratch on your computer.

## Install

1. Put `scratch.pax` on SD at `/sd/roms/pax/scratch.pax`, or launch it from the PAX store.
2. Extract `scratch-data.zip` into `/sd/roms/scratch/`. The result must contain
   `romfs/gfx/translations/languages.json`, `projects/`, and `settings/`.
3. Add `.sb3` files to `/sd/roms/scratch/projects/`. An original `PAX Demo.sb3`
   tests SVG/PNG costumes, scripts, a variable monitor, pen trails, speech and sound.
   **PAX-Fun-Demo.sb3** adds seven live hardware pages and touch/keyboard examples for the onboard outputs. See [its controls and hardware notes](data/PAX-Fun-Demo.md).
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
implemented. Network downloads, custom Lua/native extensions,
advanced color/fisheye/whirl/pixelate/mosaic effects and clipboard are unavailable.
Player text overlays use a small ASCII font; the editor uses antialiased Noto Sans from the existing asset pack. SVG costume text uses the bundled fonts.
Date/time sensing depends on the loader's available clock and currently uses
uptime. Complex Scratch projects may exceed the device's memory/CPU budget.


## Project loading (0.4.12)

Playback reads project JSON directly from a bounded, CRC-verified ZIP-entry stream and parses sprites and saved-list entries incrementally, moving lists directly into runtime storage without retaining the full extracted project description. Block definitions stay as compact serialized JSON and are decoded individually when visited, avoiding a full block DOM alongside runtime scripts. Large saved lists and parsed block constants share immutable repeated short strings with bounded caches, preserving every entry and its type. This reduces loading peaks for projects such as ScratchBenchmark v1.0.2 without deleting saved data or changing timed workloads. On RV32, values use a compiler-managed four-byte-aligned 12-byte layout instead of 24 bytes. Rare color values share immutable full-precision color storage, so numbers, strings and Booleans do not reserve an inline four-float color. Script delete-all operations release buffers of at least 1024 entries; small lists retain capacity to avoid allocator churn. All values, types and saved entries remain intact. Playback loads hidden costumes on demand, evicts cached inactive costumes before new image allocations, and rasterizes SVGs into their final RGBA storage without a duplicate bitmap. The pen surface allocates native-resolution RGBA rows on demand instead of requiring one contiguous multi-megabyte block; clearing releases those rows. Large SVGs rasterize in bands, and noninterlaced 8-bit RGB/RGBA PNGs inflate by scanline with chunk CRC and zlib checksum checks; identical adjacent rows share storage without changing pixels. Flat/striped rows additionally store exact RGBA runs when these need less than half the raw bytes; high-entropy rows remain raw. Sampling expands at most one row per image on demand, retaining native resolution and every alpha/color sample. Other PNG encodings retain the existing decoder. Inactive collision masks release old image references. Hidden-sprite pen stamps load and draw their costume while retaining visibility. The editor still needs its editable project model; this change concerns playback loading.

## Playback input (0.4.3)

Touch and USB mouse presses now dispatch the standard **when this sprite clicked** scripts; keyboard/gamepad input dispatches **when key pressed** scripts. Device mice have a visible pointer during playback; desktop keeps its OS pointer. Letterbox margins and player controls block clicks through to underlying sprites. Fast queued keyboard taps are retained without duplicating typed text.

The native input regression is `python3 build.py --input-test`, followed by `build-host/scratch-test data/PAX-Fun-Demo.sb3`. It exercises actual touch, relative USB and absolute desktop input, cursor display, quick Space taps, and timed output cleanup against simulated hardware. The ordinary `--host` suite continues testing editor, selector and lifecycle behavior.

## Block editor (0.4.0)

The editor negotiates the PAX canvas and uses all 1024×600 pixels on Elecrow (800×480 fallback for older loaders). Extra width expands the workspace, and the stage, toolbar and footer stay anchored. Playback keeps its existing 480×360 stage surface. Choose a category in the left column, then drag or tap a palette block to add it. Palette items size themselves to their contents, wrap long labels, and leave clear space around hat tops, tabs and C shapes. Inline script elements have explicit gaps and matching text measurements. Drag a stack under another block, inside a loop/condition, or drag a round/green reporter into an input. A red line marks a compatible drop target. Dragging a middle block carries the blocks below it and any nested scripts.

Tap an input to change its value using the on-screen keyboard or USB keyboard. Connected reporters are drawn inline; tap their own sockets to edit their inputs. **Shift** supplies capital letters and symbols. Select sprites on the right; arrows page through larger projects. **Sprite +** adds a simple sprite with a built-in beep sound. **Var +** creates a sprite variable. Existing project costumes and sounds can be selected by their names in the relevant block inputs.

The **green flag** or **F5** exports a temporary snapshot and opens it in the player. Touch the **red stop sign** in its top-right corner, press **F6** (desktop **Shift+F5** also works), or use Select+B to return to editing. The stage pane then shows the last run. **Save** creates a new `.sb3` file and preserves the original; choose a different name for another save. The matching `.sb3.json` keeps imported controls and streams archives from SD to reduce memory use.

**Copy** duplicates a selected stack. **Trash**, or dropping a stack on the palette, removes it. **Undo/Redo** restores edits (up to 20 snapshots within a 1 MiB history budget). **Pan** lets you drag the workspace, mouse-wheel scrolls it, and **Home** brings scripts back into view. Back/New confirm unsaved edits. Open preserves dirty work in a recovery draft before replacing the workspace. Load errors remain visible and keep New/Open usable. Holding Menu to exit saves an unsaved workspace as `Recovered-<timestamp>.sb3` when storage permits.

This is a native editor with Motion, Looks, Sound, Events, Control, Sensing, Operators, Variables, Pen and Board IO blocks. It supports stacks, nested repeat/forever/if/if-else, round/boolean inputs, sprite selection and exported projects. Cloud/Lua extension authoring and full browser Scratch GUI parity are outside this version. Unknown imported blocks retain their JSON and assets and are shown by opcode. The initial stage thumbnail is approximate; Run uses the real interpreter and renderer.

### Lists and named inputs

**Variables > List +** creates a list and the palette exposes all twelve supported list blocks. Tapping named inputs opens a paged selector for variables, lists, messages, costumes, sounds and keys; keyboard entry remains available. Stage-owned variable/list IDs and existing message IDs are retained. Select the Stage to create a shared variable/list there.

### Sprite properties and assets

**Properties** edits the selected sprite's name, X/Y, size, direction, visibility, costume and rotation style. For Stage it offers name and backdrop. Values are validated and changes participate in Undo and SB3 export.

Inside Properties, **Costumes/Backdrops** and **Sounds** open asset browsers with rename, delete, select/preview and import actions. Put SVG/PNG images or WAV/MP3 files under `/sd/roms/scratch/assets/` (desktop: `pax-dev/desktop/sd/roms/scratch/assets/`). Files are limited to 8 MiB; PNG editing/import is limited to 1024x1024 pixels. At least one costume/backdrop must remain.

**Paint a copy** opens a 32x32 pixel-art canvas. Use the brush, eraser, fill, mirror, clear and stroke Undo/Redo controls, then **Apply copy** to add a content-addressed SVG costume. The original stays intact, including an imported vector original. Painted copies retain the source bitmap resolution and rotation pivot. Brush strokes finish at the release point, even when desktop motion events are coalesced.

**New** in Sounds adds a one-second tone. **Edit sound** shows a waveform with selection percentages, trim, reverse, louder/quieter, fade in/out, Play/Stop and Undo edit. WAV and MP3 files are decoded and edits export mono 16-bit PCM WAV, capped at one million sample frames. Original bytes remain available to Undo. Generated assets have actual MD5 filenames; unused generated revisions are omitted from saved archives. After 8 MiB of in-memory asset revisions, save and reopen before making more edits.

Microphone recording is unavailable: the current ABI provides audio output and microphone control/status, but no PCM capture service. Neither simulated recordings nor SDK/firmware layout changes are introduced.

### Custom blocks and zoom

**My Blocks > Make a Block** creates a procedure with up to eight named text/number or Boolean inputs and optional fast mode. Calls and parameter reporters appear in the palette; drag statements under the pink definition and reporters into its body. Clicking a definition opens its editor. Renaming/adding inputs updates existing calls and body reporters; existing input types are retained. Duplicated definitions get unique procedure codes.

The footer **- / + / percentage** controls zoom the workspace from 50% to 200%; percentage resets it to 100%. Blocks and fonts scale together, and dragging, snapping and nested input hit testing use the inverse transform. Toolbars, palettes, stage and modal dialogs stay at native resolution.

## Board I/O

See the firmware workspace documentation in `pax-esphome/docs/scratch-hardware.md` for channels, units, ranges, profiles and pin conflicts. Board IO reads configured ESPHome sensors and queues actuator commands. Sensor values are not simulated when firmware support or a sample is missing. The camera uses an optional experimental profile.

## Source and builds

`core` links to the independently versioned pinned ScratchEverywhere checkout in `upstream/`. The official browser-editor reference is kept separately in `upstream/scratch-editor-reference`, pinned in `provenance.json`; it is not part of the native binary.
`port/` owns the PAX adapters. `pax_editor_model.cpp` owns block graphs, history and SB3 export; `pax_editor_ui.cpp` owns the touch interface; `pax_editor_assets.cpp` owns image/audio edits and immutable assets, and `pax_editor_custom.cpp` owns procedure metadata. `deps/` keeps exact dependency snapshots and their
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

Scratch-exported SVG costumes with a zero-width or zero-height viewport load as a transparent texel. Their blank texture remains transparent when scaled. Malformed SVG files still report parse errors. This PAX-only runtime adjustment is reproduced by `patches/0002-pax-empty-svg.patch`.

Playback has a Turbo ON/OFF button and F7 shortcut. Turbo bypasses visual-block redraw waits while elapsed-time waits/timers remain active. F6 or the stop icon returns to the editor or picker. Normal mode remains the default unless the project requests turbo. List titles, footer lengths, and add buttons use PAX top-origin text positioning.

0.4.2 playback uses the native panel framebuffer and the already bundled NotoSans-Medium.ttf for cached antialiased text (pixel-font fallback if the font file is missing). Turbo removes per-step sleeps and yields every 4 ms instead; Scratch waits/timers still use elapsed time. Normal mode keeps its existing scheduling.

Turbo also waits one frame interval after a completed redraw before drawing again, so a slow LCD transfer cannot consume every interpreter step. Text baseline uses cap-height padding to match the UI top-origin convention.

Variable-monitor labels/values are raised one native pixel. List item text and row numbers share an ink-bounds-centered row position, preserving the approved title/footer positions.

Audio playback advances missed source samples after long interpreter stalls so play-until-done follows elapsed time instead of stretching a clip. ZIP audio load errors propagate to sound blocks. Benchmark scripts and timing checks are unchanged.


## Cloud networking (0.4.14)

PAX supports the Mist/TurboWarp cloud-variable protocol over its loader's verified TLS services. It polls cooperatively on the interpreter task and closes the connection when the project exits. Cloud networking starts disabled. Enabled projects show a responsive connecting screen before green-flag scripts start, until the first connection succeeds or fails (with a 60-second maximum). This lets projects that create cloud-dependent menu controls once observe the initialized cloud state. If the connection fails, scripts start offline. Stop/exit still works while connecting. To enable a project, add `"cloudEnabled":true` inside its `.sb3.json` sidecar's `settings` object. `cloudHost` optionally chooses a `wss://` endpoint (default `wss://clouddata.turbowarp.org/`); explicit `ws://` is available for a local test server. `cloudProjectId` optionally chooses the server namespace; otherwise the upstream `ScratchEverywhere/hash-...` identifier is derived from costume/sound asset IDs. Do not reuse a namespace for an unrelated project. The saved anonymous `playerNNNNNNN` name is stored in `scratch/cloud-username.txt`.

This sends the namespace, anonymous username and script-written cloud values to the selected server. TurboWarp's service is separate from Scratch's authenticated cloud/leaderboards, and it resets data after inactivity/server restarts. A compatible server and namespace must contain the data expected by a given project; enabling transport does not join a Scratch-account leaderboard. Server close codes and connection failures are logged, including missing firmware TLS support. There is no insecure fallback for `wss://` certificate failures.

The WebSocket implementation verifies the upgrade accept digest, masks client frames, handles fragmented/newline-delimited JSON and ping/pong, preserves numeric strings, rate-limits updates to ten per second, bounds frames/queues, and reconnects with backoff. Cloud defaults saved in the SB3 are cleared locally only after a verified connection, before server updates are applied; they are never uploaded as initial leaderboard data. This prevents stale offline cloud snapshots from overwriting live records. Cloud callbacks only update variables declared as cloud variables. Editor cloud-block authoring remains outside this version.

Protocol references: https://github.com/ScratchEverywhere/mistpp and https://github.com/TurboWarp/cloud-server/blob/master/doc/protocol.md. Server rules: https://docs.turbowarp.org/cloud-variables.

## Editor responsiveness and hardware demo (0.4.15)

Idle/hovering in the editor reuses its last raster and updates the USB mouse cursor without rebuilding block SVGs. Cached frames still present regularly to satisfy the loader frame watchdog. Horizontal and vertical workspace scrollbars reflect the full stack bounds, including nested blocks and negative coordinates, and support touch/mouse thumb dragging and track clicks. Sprite list rows use their original layout anchor, avoiding a second footer translation at 1024x600 that overlapped Properties.

Rendering pumps audio between text and image operations so short sounds are not entirely skipped during a slow UI frame. The hardware demo now exercises continuous-servo direction/speed with +60%, stop, -60%, stop; its sound example allows amplifier startup and buffered playback before restoring enable. Hardware logs distinguish queued commands from physical feedback. Actual servo motion and audible speaker output still require physical confirmation.

## Player rendering and scheduling (0.4.16)

Unrotated opaque/alpha images map horizontal source coordinates once per draw and obtain one decoded source scanline per output row. Opaque rectangles use clipped RGB565 row fills. Other transforms keep the reference rendering path. Pixel regressions compare scaled/flipped/cropped/transparent output with that reference. Player composition timing is logged separately from the loader's display transfer time.

Long immediate/warp block chains yield a resumable interpreter slice after the next 64-block time check exceeds four milliseconds, allowing the main runtime to service input/audio/networking. This bounds chains rather than changing Scratch wait durations or benchmark timing checks. A single expensive block or display transfer can still delay input.

The Elecrow firmware must keep its GPIO25 servo at 50 Hz on LEDC channel 2 (timer 1), with the 30 kHz GPIO20 LCD backlight on channel 0 (timer 0). Automatically allocating adjacent channels 0/1 makes the outputs share a timer and can replace the servo frequency with the backlight frequency. Updating the PAX app alone cannot repair that timer conflict.

## Project picker (0.4.17)

The opening SB3 picker sizes its rows to the canvas (11 rows at 1024x600; eight at 800x480), shows the visible range and total project count, and has a vertical scrollbar. Use the mouse wheel or drag/click the scrollbar to reveal additional files. Clicking a filename edits that exact project; pointer actions take priority over coincident controller confirmation. Arrows retain a visible selection, Enter or Play runs it, and E or Edit edits it. Long filenames use the available screen width before truncating.
