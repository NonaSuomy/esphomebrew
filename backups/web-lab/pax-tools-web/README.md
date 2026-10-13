# PAX Web Lab

The active launcher and NetSurf port can be compiled from source to WebAssembly. The web build fills the existing PAX service table with browser display, input, file and HTTP services. It does not execute ESP32 RISC-V `.pax` binaries.

## Build and run

```sh
cd ~/code/pax
python3 pax-tools/web/build.py --app launcher
python3 pax-tools/web/build.py --app netsurf
python3 pax-tools/web/serve.py
```

Open http://127.0.0.1:8767/ and choose **Start launcher** or **Open NetSurf**. NetSurf can also be launched from its entry in the launcher, with return to the launcher after exit. Set **NetSurf start page** to `http://127.0.0.1:8767/test-page.html` for a local HTTP check.

The builder uses `EMCC`, `emcc` on PATH, or `~/code/emsdk/upstream/emscripten/emcc`. Pass `--emcc` to select another toolchain, `--out` for another output directory and `--jobs` to change compiler concurrency. Tested with Emscripten 5.0.7. The launcher uses the workspace's shared LVGL checkout. NetSurf uses its existing pinned manifest, patched source cache and generated fonts/resources. If that cache has not been prepared, run the normal NetSurf PAX build first:

```sh
source pax-tools/native-env.sh
python3 pax-modules/tools/build_pax.py netsurf --out /tmp/pax-netsurf-build --jobs 8
```

Generated web files are in `pax-dev/web/`; compiler objects and dependency records are in `pax-tools/.cache/web/`. Both are excluded from Git. Source files live in `pax-tools/web/`. The builder records compiler, source folder and ABI header hash beside each target. Build both targets before packaging.

## Browser SD

The `/sd` filesystem is mounted through Emscripten IDBFS and saved in this browser's IndexedDB. It is separate from `pax-dev/desktop/sd/` and from any physical device SD card. It persists across app restarts and page reloads on the same origin. Different browsers, hostnames or ports have separate storage; clearing site data removes it.

Start an app before importing a folder, saving or exporting its SD. **Import SD folder** accepts a folder whose contents represent the SD root, such as `pax-dev/desktop/sd/`. It preserves existing files. **Save SD** persists current files, and exiting an app also saves. **Export SD backup** downloads a JSON snapshot of the virtual files. User files remain in this browser; import does not upload them to a server.

The launcher sees a browser-module reference named `netsurf.pax` with JSON listing metadata. This reference is not an ABI-1 executable and must not be copied to an ESP32. The browser routes that entry to `netsurf.wasm`. Imported ESP32 binaries can be listed but cannot execute in this build. Additional apps require their own source builds and any missing services.

## Browser services and limits

RGB565 display, dirty rectangles and scaled frames are drawn through SDL's browser canvas. Pointer events are mapped using the displayed canvas bounds and queued so short taps survive app polling. Keyboard arrows and Enter navigate the launcher. WASD supplies gamepad directions; J/K supplies A/B, Q/E L/R, F1 Start and Tab Select. Escape or **Stop app** exits. Fullscreen is available through the page.

Emscripten Asyncify fibers provide cooperative app tasks. Each has its own C stack and yields through the PAX delay service. This supports NetSurf's frontend and DNS task without SharedArrayBuffer, cross-origin isolation headers or hardware thread priorities. It is not an ESP32 CPU emulator.

HTTP requests are reconstructed from the app's TCP/TLS request bytes and performed with browser `fetch`. DNS callbacks return opaque browser hostname tokens; these are not real network addresses. Native NetSurf IPv4 parsing is adapted only in the generated browser source so numeric addresses use this same route. HTTPS certificate verification is performed by the browser. GET, HEAD and POST are supported. Requests omit credentials, and cookies/authentication from the PAX port are not forwarded. Non-HTTP TCP, UDP, listening sockets and raw device access are unavailable.

Cross-origin sites must allow CORS. An HTTPS page may be unable to fetch an HTTP development server because of mixed-content or local-network restrictions. Use a same-origin local server for development. The bridge buffers responses up to 64 MiB; it does not provide streaming media. NetSurf retains the current port's lack of JavaScript. Browser audio, ROM ZIP extraction, store installation, firmware flashing and execution of unchanged `.pax` binaries are not implemented here. Unsupported optional service pointers remain null.

The PS2 Web Recomp project is an architectural reference for workers, AudioWorklet and GPU rendering, not a dependency of this build. The current PAX UI is a software RGB565 framebuffer; this version does not require WebGPU.

## Checks

```sh
node pax-tools/web/test_transport.js
```

This checks split HTTP headers and POST bodies, response size/header framing, TLS EOF, invalid methods and fetch errors. Actual browser checks cover launcher rendering, keyboard navigation, NetSurf's welcome page, local HTTP rendering, persistent bookmarks and clean exit. The source adapter leaves the active hardware NetSurf port unchanged.
