# P4 Red Alert memory recovery — 2026-10-23

The Elecrow ESP32-P4 AIO and Waveshare ESP32-P4 NANO with the Raspberry Pi 4.3-inch DSI display now have public installer images built with the shared PAX memory and media fixes.

PAX resizes its three working framebuffers to the selected app canvas. Red Alert uses 640×400; on the 1024×600 AIO this releases 2,150,400 bytes of PSRAM while retaining the native display scanout buffers. Allocation is transactional: a failed growth keeps the existing buffers and canvas intact.

The shared firmware also includes guarded audio startup, shorter P4 I2S DMA buffers, and corrected native RGB565/RGB888 presentation. These fix the previously observed AIO multiplayer allocation failure and NANO intro colour/audio problems.

## Validation

The user confirmed a complete networked Red Alert game between the AIO using Wi-Fi and the NANO, without an issue. The AIO menu showed approximately 25 presented FPS and 4,042,580 bytes of free PSRAM. This is evidence for this game and hardware pair, not exhaustive testing of every game or board.

The public images use provisioning configurations without saved network credentials. Their shared runtime was tested on the devices; these freshly compiled public provisioning images have not themselves been flashed during publication. Other installer profiles retain their previous builds.

The flash kits include the relevant source references, shared loader sources, patched display/audio source patches, and validation details. No game data or network credentials are included.

## Game-data installer action

The launcher shows GET GAME DATA only when a valid installation recipe is embedded in the local app JSON sidecar. Recipes exist for Red Alert, Doom, Quake, Duke Nukem 3D and other games. An old or missing sidecar can hide the action.

Remote GitHub Store information pages currently hide this action because the separate Recipe app accepts a local JSON path; the remote metadata is held in memory rather than saved as a file. This is a launcher limitation. Install the app with its matching JSON sidecar, then open its information page from Storage to use its recipe.
