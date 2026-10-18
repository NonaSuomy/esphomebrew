# Device setup by IP address or .local hostname

All 19 public installer firmware profiles include the shared setup-page address
validation fix. M3 LCD101 was published first; this update rebuilds the other 18
images and flash kits.

Open `http://DEVICE_IP:8080/pax` or
`http://DEVICE_NAME.local:8080/pax`. The firmware accepts the device address of
the actual connection, including an interface other than the preferred network
interface. Both IP-address and mDNS access remain supported.

The same session token, Host, port and Origin restrictions continue to protect
file-transfer commands. Unknown or misleading Host names are not accepted.

Each board retains its configured display, touch mapping, flash size, chip
revision and CPU frequency. Dual networking is enabled only by the profiles
whose board configuration provides both Ethernet and hosted Wi-Fi. Other
profiles retain their single-interface configuration.

The images compile and the installer validation checks verify all binary hashes,
flash-kit contents and manifests. Hardware testing remains pending wherever the
installer already labels a profile untested; successful compilation is not a
claim of hardware verification.

The exact source fix and regression coverage are in
[pax-device-setup-secondary-interface.patch](pax-device-setup-secondary-interface.patch).
Each refreshed flash kit includes the setup component source under
`source-reference/components/pax_web/` and the address regression test. The
`web-installer/build-info.json` records identify the compiled source revisions
and factory image SHA-256 values. Superseded local artifacts were backed up
before replacement.
