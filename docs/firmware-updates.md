# Updating PAX firmware from GitHub

Open Settings in Launcher, scroll to P4 FIRMWARE (GITHUB), and choose CHECK FOR UPDATES. The status identifies this device's board/display profile and installed GitHub Actions build ID. If a newer compatible build is available, choose INSTALL UPDATE, then CONFIRM & RESTART. Keep power connected until the device restarts.

The same controls are available at `http://DEVICE_IP:8080/pax`. Checking does not install anything. Installation requires a second confirmation bound to the exact checked build. Updates are downloaded by the device over HTTPS; no computer or SD staging space is needed.

Updates write only the inactive application partition. They preserve NVS, provisioned Wi-Fi, saved network configuration, brightness/volume settings, and all SD-card files, including game data and launcher preferences. The running app closes and pending preferences are synchronized before installation. The boot partition changes only after the complete image passes SHA-256 and ESP image validation. A download/write/verification failure leaves the existing boot firmware selected; check again to retry.

Each CI build publishes `install/updates/PROFILE.json` alongside its OTA image. The device verifies the profile, ESP32-P4 revision range, complete partition layout, image size, fixed download URL, and SHA-256. It never installs a factory image or repartitions flash. A custom YAML without a registered hardware profile has no public update target. Builds use monotonic GitHub run IDs; an older or identical build is not offered as an upgrade. Local development builds report ID 0.

Existing devices need one initial OTA upgrade to firmware containing this updater. They also need the current SD starter/Launcher to expose the Settings buttons; firmware OTA deliberately leaves an existing SD launcher unchanged. The device setup web page can update firmware independently of the SD launcher. Updates do not migrate applications, replace SD files, or silently install at startup.

Public updates use the configuration for that installer profile. Custom compile-time integrations belong in a custom build; runtime settings remain on the device. Physical tests and compilation coverage are reported separately.
