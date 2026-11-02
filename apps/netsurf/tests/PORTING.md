# Switch fork integration notes

Reference checkout: `upstream/netsurf-switch`, commit `29d910fc28e6e0c816eea4a86bae4e40f2ac699d`. The production PAX manifest currently fetches NetSurf and libcss separately from netsurf-browser and does not build the fork's QuickJS frontend or media integration. This reference checkout is a combined tree; its directories are not individually fetched by the current PAX build recipe.

The fork adds coupled layout, parser, DOM, QuickJS and media changes. PAX's loader, framebuffer, resources, input, HTTP, bookmarks, and JPEG accelerator patches must be adapted to that tree. A dry-run against an isolated fork copy showed conflicts in patches 0003, 0007, 0009 and 0013. A sequential trial produced one rejected hunk each; patch 0009's target already includes the desired initialization. Recheck all patches against a clean tree before relying on this result, since some depend on prior patches.

Suggested sequence: introduce a reproducible manifest pinned to the fork and its matching modified libraries; merge and compile the PAX frontend; validate CSS with `features.html`; integrate QuickJS and generated bindings with explicit memory and execution limits; port inline media using PAX player services and test media lifetime, audio and frame scheduling. Preserve the current production build until the new build has passed device tests. Avoid enabling unrestricted script execution without resource limits on this device.

The feature page tests flexbox, grid, custom properties, gradients, event-driven DOM updates, timers and inline media. The external MP4/MP3 require network connectivity; use local equivalents to isolate decoding from transport.

## Trial build results

A trial overlaid the fork's core and libraries into a disposable build tree, then applied the existing PAX integration patches with local conflict adjustments. The resulting 799-unit compilation reached the link step. It is not a reproducible or deployable build yet: the current recipe omits the fork's framebuffer-effects implementation and canvas support; the newer HTML code also needs `js_exec_module` and `js_set_importmap` even when JavaScript is disabled. The Switch frontend calls its own audio shutdown and webfont registration, which need proper PAX implementations or feature-gated paths. The libcss recipe also compiles both generated and fork-provided `background_image` parsers, producing a duplicate symbol. Compiler warnings flagged changed bookmark and webfont interfaces that must be resolved before device testing.

The upstream QuickJS engine source is not in the Switch checkout; its Switch build downloads quickjs-ng separately, so a pinned RISC-V build and NetSurf bindings must be added. Inline media additionally needs a shared in-process player integration, not the current whole-app handoff. The Switch hardware video decoder is not compatible with P4.

## JavaScript trial

QuickJS-ng v0.17.0 resolves to `6d46d07d04041b40f4f49eaa7fdebe44c314c699`. The engine and all five fork-specific binding files compile for the P4. A manual trial link succeeded after supplying the missing framebuffer effect object, the fork's non-Switch audio stubs, and the JavaScript content source. The trial excludes the PAX webfont and canvas callbacks, disables QuickJS atomics, and is not suitable for deployment. All engine objects must use `-march=rv32imafc_zicsr_zifencei -mabi=ilp32f`; otherwise the linker rejects mixed floating-point ABIs. QuickJS-ng must disable atomics for this preliminary single-threaded build (`__STDC_NO_ATOMICS__=1`) or provide a proper pthread/64-bit atomics implementation. The fork uses a 64 MiB QuickJS heap limit, which exceeds the available device memory; choose and test a lower bound.

`quickjs_smoke.c` tests basic JavaScript evaluation, a 4 MiB memory limit and interrupt-driven termination against QuickJS-ng v0.17.0 on a host; this does not validate the RISC-V runtime, DOM bindings or device memory usage. The modern fork's `fbfx.c` only accepts 32-bit libnsfb surfaces. PAX currently uses a 16-bit RGB565 framebuffer, so gradients, rounded shapes and related effects need an RGB565 implementation or a carefully measured conversion buffer before the modern CSS port can be considered working.

## RGB565 effects and script limits

`switch-rgb565.patch` adds RGB565 reads, writes, blending, gradient rendering, layer save/restore and video blitting to the fork's 32-bit framebuffer effects. `test_fbfx_rgb565.py` applies the patch to the pinned local Switch checkout, compiles a host test and checks RGB565 colours, blending, padded rows, gradients, layer restoration and a 32-bit pixel. The patched effect object also compiles and links for P4 in the trial. These checks do not establish full CSS correctness or P4 runtime performance.

`switch-quickjs-limits.patch` changes the PAX build of the Switch backend to use a 4 MiB limit per QuickJS runtime, a 128 KiB JS stack limit and a maximum three-second script interrupt deadline. It compiles and links in the P4 trial; the host QuickJS smoke test covers engine-level limits only, not browser or device behavior. The cap is per runtime and does not limit allocations outside QuickJS. Neither patch is wired into the production manifest yet.

## Experimental build and emulated checks

`tools/build_netsurf_modern.py --quickjs /path/to/quickjs-ng` builds a separate experimental binary using the pinned Switch and QuickJS checkouts, `switch-pax-adapt.patch`, `switch-quickjs-limits.patch` and `switch-hubbub-quiet.patch`. The production manifest stays unchanged. The PAX frontend enables JavaScript only when the experimental build flag is present. PAX's own bookmark initialization is used once; Switch-specific webfonts, canvas and inline media are not wired up.

The version staged as `netsurf-modern.pax` at SHA-256 `13bcdcdbadb02b7109473929cbd304229e031a92b6648498b0f934f484fb67c4` includes an additional launcher-path change. An earlier build with the same rendering and JavaScript code loaded `features.html` in RISC-V emulation and visibly rendered flexbox, grid, rounded corners and a gradient. A later build with the parser debug hook disabled ran `javascript.html`, visibly changed its heading to `JAVASCRIPT WORKS`, and exited cooperatively in RISC-V emulation. The staged binary has not been tested on the device, and the emulator's timings do not reflect P4 performance. Inline media remains unfinished.

The staged SHA above was also run through `pax-tools/web/test_emulator.cjs` with `PAX_TEST_BINARY` pointing at the staged PAX and `PAX_TEST_HTML` at `javascript.html`. Its rendered heading read `JAVASCRIPT WORKS`, and it exited cooperatively. This checks a simple inline script and DOM text update in emulation; it does not establish device stability or general web compatibility. The test runner accepts those environment variables to repeat the experiment with other binaries and HTML files.
