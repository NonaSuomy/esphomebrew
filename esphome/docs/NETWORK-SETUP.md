# Three-button network setup

Choose **NETWORK SETUP** at the bottom of the three-button screen. No SD app or working network is required to open it.

- **AUTO** enables the interfaces compiled into the board profile. The Elecrow AIO uses Ethernet first and Wi-Fi as fallback.
- **WI-FI** enables Wi-Fi and disables Ethernet.
- **ETHERNET** enables Ethernet and disables Wi-Fi.

The interface choice is saved in device preferences and restored at boot. The AIO already includes `packages/pax-network-dual.yaml` through `boards/elecrow/aio.yaml`; no Ethernet pin changes are required. Unsupported interfaces are disabled in the panel.

The panel shows live link, DHCP/static mode, IP, mask, gateway and DNS details every second. Choose DHCP or enter static IPv4 details, then press **APPLY IP SETTINGS**. Staged fields are preserved while editing. IP settings use the existing loader network configuration service and preferences.

For Wi-Fi, choose **SCAN NEARBY**, select a network and enter its password, or enter an SSID manually. Selecting a scan result only stages the name. **APPLY WI-FI** starts the existing asynchronous connection flow; from Ethernet mode it also selects Wi-Fi. Passwords are masked by default, and **SHOW PASSWORD** can reveal the staged password. A blank password keeps the stored password for the current SSID unless explicitly edited; a newly selected open network uses a blank password.

Touch a text field to open the on-screen keyboard. USB keyboard text also works. **BACK** returns to the three buttons; Escape dismisses the keyboard first, then the panel. Connection attempts continue after leaving the panel. The selector rejects changes while a scan, connection attempt or firmware update is in progress. Wi-Fi credentials continue to use the existing SD recovery backup; restoring that backup respects a saved Ethernet-only choice.

Host regression tests compile the production selector and test interface availability, driver lifecycle, busy-operation rejection, preference saving and rollback. The Wi-Fi backup runtime test includes disabled-interface recovery. A real board test is still required for cable-link changes and credential entry.

After building the desktop profile, run `python3 tests/check_native_network_panel.py` to exercise the real LVGL panel with synthetic network data. It checks DHCP field visibility, keyboard dismissal, scan-result staging, password validation and clearing, apply, interface selection, and restoring keypad focus when the panel closes.
