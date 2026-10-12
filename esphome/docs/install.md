# Install PAX on your board

This source package targets Linux x86_64 (or Ubuntu in WSL2, with USB passed through). Use Python 3.12-3.14. It includes the modified ESPHome fork, local PAX/USB components, patched ESP-IDF 6.1 source with its submodules, and the launcher/recipe SD files. Downloads are still required for Python packages, compiler tools, PlatformIO libraries and IDF managed components. Allow several GB of disk space. Do not substitute stock ESPHome: the local fork is required.

1. Extract the ZIP and open a terminal in its `pax` directory.
2. Install host prerequisites (Ubuntu example):
   ```sh
   sudo apt install python3 python3-venv python3-dev git cmake ninja-build build-essential pkg-config libffi-dev libssl-dev unzip
   ./pax-tools/setup-firmware.sh
   ```
3. For a private Wi-Fi profile such as `elecrow`, edit `pax-esphome/secrets.yaml`: set your Wi-Fi SSID/password and a fallback hotspot password of at least 8 characters. Each owner compiles their own credentials; this package contains no private credentials or credential-bearing firmware. Set a unique `device_name` if using several boards. Public `*-web` profiles are provisioned over Improv after flashing, and Ethernet profiles do not need Wi-Fi secrets.
4. Pick the exact board (`elecrow` is the current default; `waveshare` is the ESP32-P4-WIFI6 with the original 7-inch DSI panel, hardware untested):
   ```sh
   ./pax-tools/build-firmware.sh config elecrow
   ./pax-tools/build-firmware.sh compile elecrow
   ```
   Replace `elecrow` with `waveshare`, `nano` (HDMI 720p60 / Ethernet), or `nano1080` (HDMI 1080p30 / Ethernet), or `waveshare7b` (integrated 7B / rev3) for that target. NANO does not require Wi-Fi credentials. Waveshare panel orientation, simultaneous SD/Wi-Fi and sound still need hardware validation; additional experimental keys are `nano-wifi`, `eth-hdmi720`, `waveshare4b`, `tab5`, `jc1060`, `jc4880`, `jc8012` and `m3-hdmi720`. See [shared configuration](configuration.md) and [board scope](boards.md).
5. First install via the board's programming USB port:
   ```sh
   ./pax-tools/build-firmware.sh upload elecrow --device /dev/ttyUSB0
   ```
   Your port may be `/dev/ttyACM0`; find it with `ls /dev/ttyUSB* /dev/ttyACM*`. If it cannot connect, hold BOOT, tap RESET, release BOOT, then retry. The upload command uses the configured serial flash layout. Use a data-capable USB cable and the programming port, rather than the USB host port. Give your Linux account serial-port access (typically the `dialout` group), then log in again.
6. Later updates can use OTA after Wi-Fi or NANO Ethernet connects:
   ```sh
   ./pax-tools/build-firmware.sh upload elecrow --device 192.168.0.241
   ```
   Replace the IP with **your board's** address. Both serial and OTA are supported. `upload` sends the most recent build; `run` compiles and uploads together. A factory image is for initial serial installation; the normal firmware image is for OTA. Private and Wi-Fi builds are kept under `pax-dev/firmware-local/<yaml-name>/`; Ethernet builds use `pax-dev/firmware/<yaml-name>/`. Public installer staging separately verifies the complete configuration and binary for private credentials.
7. Format an SD card FAT32; copy the **contents** of `sd-card/` to its root. This creates `/roms/pax/launcher_ui.pax` and its JSON metadata, plus `recipe.pax`. Insert it, reboot, then select Storage to launch the UI. Additional apps and legitimate data files can be installed through the store and Recipe. No game ROMs, ISOs, keys or copyrighted game assets are included.
8. To serve the included launcher from your computer:
   ```sh
   ./pax-tools/serve-dev.sh 8000
   ```
   Set the launcher's Network → development-server URL to `http://YOUR_COMPUTER_IPV4:8000/`. The computer and board must be reachable on the same network. Friendly names, dates and sizes require JSON sidecars and this metadata server.

In Launcher Settings, enable list details and choose FILE SIZE UNITS (Bytes, KB, MB, GB or Auto). KB/MB/GB use powers of 1000; fractions show two decimal places. Auto picks a suitable unit. Preferences are saved on SD and existing list settings are preserved.

The included sources and scripts have been validated on the development host. A completely new host setup remains unverified; compiler downloads require internet access. See boards.md for board validation scope and manifest.json for exact file hashes/provenance. Do not distribute your generated secrets.yaml or firmware-local outputs.

## If the Wi-Fi coprocessor does not connect

ESP32-P4 has no built-in radio. Its ESP32-C6 must already run compatible ESP-Hosted firmware for the configured transport (Elecrow SPI, Waveshare SDIO). Uploading the P4 YAML does not itself flash the C6. If boot logs show hosted RPC/transport failures, check the board's C6 firmware and wiring before retrying P4 OTA. Use the board manufacturer's C6 flashing procedure and the [ESPHome hosted firmware/update documentation](https://esphome.io/components/update/esp32_hosted/) to choose a compatible image; the exact new-board coprocessor recovery procedure has not been tested here. Serial P4 installation is still available independently of Wi-Fi.

## Browser installation and public firmware

Open https://nonasuomy.github.io/esphomebrew/install/ in a desktop browser with Web Serial support. Select your exact board/display, connect its programming USB port, acknowledge the hardware-test status, then click Connect & install. Public Wi-Fi builds use Improv Serial provisioning and a temporary setup hotspot; they contain no station credentials. The C6 still requires compatible ESP-Hosted firmware. NANO uses Ethernet DHCP. Download the SD starter or the selected board flash kit on that page.

To rebuild the public installer locally:
```sh
python pax-tools/generate-web-configs.py
./pax-tools/build-firmware.sh compile elecrow-web
./pax-tools/build-firmware.sh compile waveshare-web
./pax-tools/build-firmware.sh compile waveshare7b-web
./pax-tools/build-firmware.sh compile nano
./pax-tools/build-firmware.sh compile nano1080
python pax-tools/stage-web-installer.py
```
The staging script rejects secret-bearing configs and checks all binaries for the local credential values without printing them. GitHub Pages publishes `pax-github/web-installer/` under `/install/`; build-info.json records firmware hashes and repository provenance. Rebuild and stage before publishing changed firmware. The browser connection and public images still need hardware validation.

The Elecrow, NANO and original WIFI6 packaged images use `engineering_sample: true`, targeting ESP32-P4 revisions 0/1 (image maximum revision 199). Production rev3 requires `engineering_sample: false` and a separate compilation; do not use these prebuilt images on rev3. Check the chip revision in the boot log.

## SD setup over the local network

After flashing and provisioning Wi-Fi, choose Visit device in the browser installer. The firmware returns `http://<device-ip>:8080/pax` through Improv. NANO uses its Ethernet address. You can also enter the IP address in the public installer's Open device setup section, or open the device URL directly in a browser on the same network.

Insert a FAT32 or exFAT SD card. Return to the three-button screen, or use Close running app on the device page and wait for the app to close. Choose Install SD starter to download Launcher, Recipe and their JSON metadata from GitHub. The firmware checks each file's pinned size and SHA-256 before committing it. Internet access is required for this option.

To upload from your computer, choose a destination such as `/roms/pax`, `/roms/nes` or `/backgrounds`. Select files or a folder, review the listed paths, confirm replacement of existing paths, then upload. Folders keep their structure below the chosen destination, including the selected folder's name. These uploads need only the local network. File and folder names use letters, numbers, dots, underscores and hyphens, with no traversal segments; the maximum file size is 1 GiB.

The local page streams each file to an SD staging file using a bounded buffer. The browser hashes the file incrementally, and the device checks the received length and SHA-256, flushes the staging file, then replaces the target through the existing backup and restore path. An interrupted or rejected transfer preserves the previous target. Transfers are serialized and blocked while an app or another storage operation is running. The page shows upload progress and starter download status.

The device page is served on HTTP port 8080, alongside the Wi-Fi setup portal on port 80. The public HTTPS installer opens that page; local file uploads run from the device page itself. The HTTP interface does not bridge the native debug API into the browser and does not allow cross-origin uploads. POST requests require a per-boot session token obtained from the device page. Keep this interface on the local network.

When publishing changed starter files, run `python3 pax-tools/generate-web-assets.py` before compiling and `python3 pax-tools/stage-web-installer.py` after compiling. Firmware contains the expected starter hashes, so publishing different starter bytes requires rebuilding the corresponding firmware.

## Install apps from the browser store

Open https://nonasuomy.github.io/esphomebrew/ and use **Install on device** beside an app.

For USB, use desktop Chrome or Edge, connect the board's programming USB port, choose **Connect USB serial**, and select your device in the browser's port picker. Close any running app, confirm replacement of matching files, then choose the app. The store checks downloaded files, transfers them over serial, and waits for the device's SHA-256 verification before reporting success. The device does not need Wi-Fi for this transfer. Current PAX firmware is required; older firmware does not support serial SD uploads. Disconnect the serial session before flashing firmware or opening another serial monitor.

For Wi-Fi or Ethernet, enter the device's local IP address or `.local` name. Choosing an app opens `http://DEVICE_ADDRESS:8080/pax` with that app selected. Review its files, confirm replacement and press **Install selected app**. Keep the browser tab open until verification finishes. This handoff lets the device page use the existing local transfer service. The SD card remains inserted throughout.

Apps are saved as `/roms/pax/<appname>.pax` with their matching JSON details. Data supplied by the store goes to the paths in the app's manifest. Games and emulators that require your own ROMs or media still need those files; see the app description and use the device page's file or folder upload controls. Multi-file installs commit each verified file separately; after an interrupted install, retry to complete missing files.

All active YAMLs include shared files under `packages/`, `boards/` and `displays/`. Copy the entire source package when sharing a configuration; an entry-point YAML by itself is insufficient.
