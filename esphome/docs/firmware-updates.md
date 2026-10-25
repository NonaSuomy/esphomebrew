# Updating PAX firmware from GitHub

Open Settings in Launcher, scroll to P4 FIRMWARE (GITHUB), and choose CHECK FOR UPDATES. The status identifies this device's board/display profile and installed GitHub Actions build ID. If a newer compatible build is available, choose INSTALL UPDATE, then CONFIRM & RESTART. Keep power connected until the device restarts.

The same controls are available at `http://DEVICE_IP:8080/pax`. Checking does not install anything. Installation requires a second confirmation bound to the exact checked build. Updates are downloaded by the device over HTTPS; no computer or SD staging space is needed.

Updates write only the inactive application partition. They preserve NVS, provisioned Wi-Fi, saved network configuration, brightness/volume settings, and all SD-card files, including game data and launcher preferences. The running app closes and pending preferences are synchronized before installation. If saving pending settings fails, installation is cancelled. The boot partition changes only after the complete image passes SHA-256 and ESP image validation. A download/write/verification failure leaves the existing boot firmware selected; check again to retry.

Each CI build publishes `install/updates/PROFILE.json` alongside its OTA image. The device verifies the profile, ESP32-P4 revision range, complete partition layout, image size, fixed download URL, and SHA-256. It never installs a factory image or repartitions flash. Only the registered public installer entry-point YAML files opt in; private/custom entry points have no public update target. Builds use monotonic GitHub run IDs; an older or identical build is not offered as an upgrade. Local development builds report ID 0.

Existing devices need one initial OTA upgrade to firmware containing this updater. They also need the current SD starter/Launcher to expose the Settings buttons; firmware OTA deliberately leaves an existing SD launcher unchanged. The device setup web page can update firmware independently of the SD launcher. Updates do not migrate applications, replace SD files, or silently install at startup.

Public updates use the configuration for that installer profile. Custom compile-time integrations belong in a custom build; runtime settings remain on the device. Physical tests and compilation coverage are reported separately.

## Installed and latest versions

Settings and the device setup web page show the installed GitHub build ID and,
after a successful check, the latest published build ID. “You are on the latest
version” means the check succeeded and no newer build is available. Local builds
are labelled unversioned. A failed check reports an error and cannot establish
whether the device is current. Network/DNS failures are reported separately from
HTTP and manifest/board/partition failures. The updater restores DNS from the
active network interface when its resolver slots are empty after boot.

## C6 coprocessor firmware

C6 checking and installation are separate from P4 firmware updates. The resolved
`esp32_hosted` configuration selects an approved profile; a display's size does
not select the radio firmware. Current approved images use ESP-Hosted 2.12.12,
matching the host library, rather than the newest incompatible upstream major.

* `c6-spi-aio`: Elecrow AIO full-duplex SPI, mode 3. P4 MOSI7/MISO9/CLK15/CS14,
  handshake11/data-ready10/reset2 connect to C6 MOSI20/MISO21/CLK18/CS19,
  handshake23/data-ready22. Other SPI wiring is not automatically approved.
* `c6-sdio`: ESP32-C6 four-bit SDIO, including configured NANO, M3, Guition,
  Waveshare and Tab5 profiles. C6 SDIO slave pins are fixed by the peripheral;
  P4 host routing comes from each board YAML. One-bit/custom/other-chip
  configurations have no approved target.

The registry `components/pax_loader/c6-firmware.json` pins version, URL and full
image SHA-256. The AIO binary is built separately; ESPHome's published C6 images
are SDIO-only and are never selected for SPI. The build pipeline records the
compiled C6 target in each board's build record and update manifest. The web
page queries the installed C6 version when checking and displays it alongside
the board-recommended version and transport. The launcher defaults to
`board-recommended`; an offline path or alternate URL is accepted only if its
bytes match the same approved digest. Both prepare and install verify the full
file hash, ESP image integrity and host-library version before remote flashing.
An equal/newer installed C6 version is not offered for downgrade or reinstallation.
Checking stages a temporary file on SD; it does not flash either processor.

To rebuild the AIO candidate, activate ESP-IDF 5.5.2 and run the public
`tools/build-c6-firmware.py --component ESP_HOSTED_2_12_12_DIR --work NEW_DIR
--defaults web-installer/c6/2.12.12/aio-spi/sdkconfig.defaults`. The script verifies
the source component and effective slave pins. Review and approve the resulting
hash in the registry; build timestamps can change the binary digest. The checked
binary's build record and upstream Apache licence accompany the download.
If an older C6 does not implement the version query, an explicit recovery check
can be requested with `POST /pax/c6/check` and header
`X-PAX-C6-Allow-Unknown: yes` (plus the normal device session token). This permits
staging the approved image despite an unknown installed version. It does not
install automatically: the separate install request still confirms the checked
profile, revalidates the image, and requires a working hosted OTA transport.
A successful version query still prevents equal-version installs or downgrades.
C6 control RPCs run on the ESPHome main loop alongside Wi-Fi control RPCs.
This avoids concurrent consumers of ESP-Hosted 2.x's shared response queue;
image downloading and validation remain on the worker. Wi-Fi status polling
is deferred while an update is busy. Malformed AP-info responses return an
error instead of reaching the hosted library's null-pointer assertion.

The virtual-file equivalent is command `PAX_C6_PREPARE_UNKNOWN` (3); existing
ABI-1 request and status sizes are unchanged.

If the hosted transport or the C6's OTA partitions are unavailable, recovery
requires the board's C6 USB/UART download connection. A failed version query
alone does not establish that this recovery connection is needed.
