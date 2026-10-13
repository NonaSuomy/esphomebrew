# PAX Web Lab

The Web Lab offers original ABI-1 RISC-V `.pax` execution through Unicorn WebAssembly, plus source-compiled launcher and NetSurf previews. Both modes use the existing PAX service table and a browser virtual SD card.

## Build and run

```sh
cd ~/code/pax
python3 pax-tools/web/build.py --app launcher
python3 pax-tools/web/build.py --app netsurf
python3 pax-tools/web/serve.py
```

Open http://127.0.0.1:8767/ and choose **Start PAX emulator** for original binaries and verified GitHub store installation. Choose **Source launcher** for the source-built preview. Open NetSurf through its entry in the launcher, with return to the launcher after exit. Enter addresses inside NetSurf; `http://127.0.0.1:8767/test-page.html` provides a local HTTP check.

The builder uses `EMCC`, `emcc` on PATH, or `~/code/emsdk/upstream/emscripten/emcc`. Pass `--emcc` to select another toolchain, `--out` for another output directory and `--jobs` to change compiler concurrency. Tested with Emscripten 5.0.7. The launcher uses the workspace's shared LVGL checkout. NetSurf uses its existing pinned manifest, patched source cache and generated fonts/resources. If that cache has not been prepared, run the normal NetSurf PAX build first:

```sh
source pax-tools/native-env.sh
python3 pax-modules/tools/build_pax.py netsurf --out /tmp/pax-netsurf-build --jobs 8
```

Generated web files are in `pax-dev/web/`; compiler objects and dependency records are in `pax-tools/.cache/web/`. Both are excluded from Git. Source files live in `pax-tools/web/`. The builder records compiler, source folder and ABI header hash beside each target. Build both targets before packaging.

## Browser SD

The `/sd` filesystem is mounted through Emscripten IDBFS and saved in this browser's IndexedDB. It is separate from `pax-dev/desktop/sd/` and from any physical device SD card. It persists across app restarts and page reloads on the same origin. Different browsers, hostnames or ports have separate storage; clearing site data removes it.

Start an app before importing a folder, saving or exporting its SD. **Import SD folder** accepts a folder whose contents represent the SD root, such as `pax-dev/desktop/sd/`. It preserves existing files. **Save SD** persists current files, and exiting an app also saves. **Export SD backup** downloads a JSON snapshot of the virtual files. User files remain in this browser; import does not upload them to a server.

In source mode, the launcher sees a browser-module reference named `netsurf.pax` with JSON listing metadata. This reference is not an ABI-1 executable and must not be copied to an ESP32. The browser routes that entry to `netsurf.wasm`. Emulated mode moves this old reference to a hidden reference folder and seeds real Touch test and launcher executables. Imported ABI-1 binaries can run if their required services are supported. The GitHub store verifies binary size, SHA-256 and ABI, downloads declared data files with size/hash verification, and preserves replaced files under `/sd/.pax-backups/`. The browser must permit CORS for every download. Source mode does not provide this installer.

## Browser services and limits

RGB565 display, dirty rectangles and scaled frames are drawn through SDL's browser canvas. Pointer events are mapped using the displayed canvas bounds and queued so short taps survive app polling. Keyboard arrows and Enter navigate the launcher. WASD supplies gamepad directions; J/K supplies A/B, Q/E L/R, F1 Start and Tab Select. Escape or **Stop app** exits. Fullscreen is available through the page.

Emscripten Asyncify fibers provide cooperative app tasks. Each has its own C stack and yields through the PAX delay service. This supports NetSurf's frontend and DNS task without SharedArrayBuffer, cross-origin isolation headers or hardware thread priorities. This describes source mode. Emulated mode executes RV32 instructions, provides separate guest task stacks and CPU contexts, and preempts instruction batches so audio tasks cannot starve rendering. It emulates the PAX ABI rather than an entire ESP32 board.

HTTP requests are reconstructed from the app's TCP/TLS request bytes and performed with browser `fetch`. DNS callbacks return opaque browser hostname tokens; these are not real network addresses. Native NetSurf IPv4 parsing is adapted only in the generated browser source so numeric addresses use this same route. HTTPS certificate verification is performed by the browser. GET, HEAD and POST are supported. Requests omit credentials, and cookies/authentication from the PAX port are not forwarded. Non-HTTP TCP, UDP, listening sockets and raw device access are unavailable.

Cross-origin sites must allow CORS. An HTTPS page may be unable to fetch an HTTP development server because of mixed-content or local-network restrictions. Use a same-origin local server for development. The bridge buffers responses up to 64 MiB; it does not provide streaming media. NetSurf retains the current port's lack of JavaScript. Emulated mode supports stereo PCM WebAudio after **Enable audio**, stored/deflated ROM ZIP extraction with CRC and bounds checks, verified store installation, and execution of unchanged ABI-1 binaries. Performance and service coverage vary between apps. Firmware flashing is unavailable here. Unsupported optional service pointers remain null.

The PS2 Web Recomp project is an architectural reference for workers, AudioWorklet and GPU rendering, not a dependency of this build. The current PAX UI is a software RGB565 framebuffer; this version does not require WebGPU.

## Checks

```sh
node pax-tools/web/test_transport.js
```

This checks split HTTP headers and POST bodies, response size/header framing, TLS EOF, invalid methods and fetch errors. Actual browser checks cover launcher rendering, keyboard navigation, NetSurf's welcome page, local HTTP rendering, persistent bookmarks and clean exit. The source adapter leaves the active hardware NetSurf port unchanged.

## Original binary checks

Unicorn.js 2.1.4 is pinned to Git commit `560284c59c11c8d6c94d537f4354339f069ddcdf`. Its source checkout is `pax-tools/upstream/unicorn-js/`. Staging verifies the npm package integrity and exposes its live WASM heap for bounded memory transfers. The original binary payload and ABI layout remain unchanged.

```sh
python3 pax-tools/web/stage_emulator.py
node pax-tools/web/test_emulator.cjs touchtest
node pax-tools/web/test_emulator.cjs launcher
# After building launcher_ui with pax-recipe/build-launcher.sh:
node pax-tools/web/test_emulator.cjs launcher-isolation
node pax-tools/web/test_emulator.cjs nes-zip
node pax-tools/web/test_emulator.cjs doom
node pax-tools/web/test_emulator.cjs install
```

The NES test generates a small mapper-0 test ROM. Doom uses the existing verified free shareware files under `pax-dev/desktop/sd/roms/doom/`. These tests check rendering, cooperative exit, task services, ZIP extraction and installer rejection. They do not establish compatibility for every app or prove real-time audio performance. The engine currently runs in bounded main-thread batches, with Canvas2D rendering and WebAudio buffers. Workers, AudioWorklet and WebGPU remain future work.

## Performance checks

The emulator uses larger bounded CPU batches, reuses register-transfer memory, saves CPU contexts only when another guest task actually runs, and avoids promises for synchronous ABI services. Rendering retains pixel buffers and a RGB565 color table, converts only dirty UI regions, and reuses scaled rows. Direct guest framebuffer writes still trigger a complete conversion. Resolution and color output remain the same.

```sh
node pax-tools/web/test_render.cjs
PAX_TEST_FRAMES=16 node pax-tools/web/test_emulator.cjs nes-zip
node pax-tools/web/test_emulator.cjs doom
```

On the development computer, 100 full 960x600 rendering updates took 1439 ms before and 308 ms after. One hundred 48x44 updates took 584 ms before and 2.5 ms after. These service benchmarks include scaling and color conversion; the test checks byte-identical output, fractional scaling, clipping, swapped pixels and direct framebuffer writes. The original Doom boot plus eight frames took 58.2 seconds before and 42.8 seconds after in the recorded runs. Whole-app performance still depends on emulated CPU work and guest timing. Reports include elapsed time, CPU execution time and the largest CPU batch.

The 16-frame generated NES ROM and ZIP tests pass. A longer 120-frame stress run exposes a WASM memory access error in both the previous and optimized runtimes. This remains an unresolved long-session limitation. Results and logs are recorded in `validation/browser-performance.json` at the workspace root.
