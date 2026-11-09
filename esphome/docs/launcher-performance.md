# Launcher UI performance and touch layouts

Launcher UI 0.6.3 retains five recycled app-row widgets, asynchronous I/O and
progressive metadata. The changes below preserve the ABI-1 service layout.

## Display handoff

The MIPI driver observes the actual DMA interrupt core. A buffer submitted on
that same core can be released after one subsequent frame-complete callback:
the submitter cannot interleave with the interrupt between its buffer selection
and callback. A different-core submission, or one before the interrupt core is
known, retains the two-completion guard. Interrupts before submission and failed
submissions do not release the previous front buffer.

This removes an unnecessary wait on the normal same-core path. It is not a
measured FPS claim: physical frame pacing and tearing still require a board test.

## Catalog discovery and memory

Compatible firmware recognizes `@pax-dir-v1:<offset>:<path>` in the existing
`file_list_dir` service. Each request returns at most 32 entries in a bounded
buffer and closes its directory before returning. The worker/UI mailbox runs
between pages, allowing cancellation and navigation. There is no persistent
iterator to retain across USB unplug or app shutdown. An unavailable page is an
error rather than a successful truncated listing. Changes to a directory during
a scan can require Refresh, since offsets follow directory enumeration order.

Older firmware falls back to the existing single-call listing and retains that
firmware's buffer limits. New firmware is needed for complete large-directory
discovery. Filenames whose full launch path cannot fit are rejected rather than
silently truncated.

Catalog strings/stat records allocate PSRAM starting at 128 records and grow
geometrically to a 2,048-record safety limit. Each source refresh releases the
old allocation; app shutdown releases the remaining allocation. Failed growth
preserves existing records. A `+` beside the count identifies a capacity or memory
limit. Global filtering and stable index sorting still cover all loaded records;
this is paged enumeration with a retained catalog, not separately sorted pages.
The merge sort takes O(n log n) comparisons. The title cache and favorites remain
bounded separately at 192 entries each.

## Touch layouts

Source/Settings buttons and Settings action controls have a 48-physical-pixel
minimum target. Source buttons have at least 8 pixels between them. On panels
with less than 760 content pixels, Settings labels stack above wider controls;
fonts are not reduced to fit two columns. The title and Back button remain fixed
while the Settings list scrolls. Mouse-wheel navigation clamps to content bounds.
The keyboard reserves at least 224 pixels where panel height permits, so its
four key rows remain comfortable touch targets. The informational brand is
hidden if the full name cannot fit readably beside controls/status.

The single I/O worker polls every 10 ms when idle rather than every 2 ms. USB
mass-storage already releases the mounted VFS/device on disconnect, and Settings
widgets are already torn down on leaving the page. The network drivers remain
unchanged; their hotplug/reconnect resources are not removed to save RAM.

## Verification

Tests exercise 700 long filenames, bounded pages, duplicates, allocation failure,
scan cancellation, sort stability, cross-core display handoff and real LVGL
layout. Compiled RISC-V emulator checks cover keyboard/widget lifetime. A board
trial should check AIO and Nano touch scrolling, typing, SD/USB removal during a
scan, and physical display pacing. No device was flashed as part of this change.
