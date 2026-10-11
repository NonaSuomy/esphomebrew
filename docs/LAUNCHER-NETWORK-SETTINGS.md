# Launcher Network settings

Open Settings with Select. The Network section contains DHCP, device IP,
subnet, gateway and DNS; SAVE NETWORK applies those device settings. Use DHCP
on a new Wi-Fi network unless you have an address reserved for this device.

DEV SERVER URL edits the computer's PAX server URL, for example
`http://192.168.0.162:8000/`. Start `pax-tools/serve-dev.sh` on that computer.
The URL is saved on SD in `roms/pax/.launcher_ui_network_url.txt`. Use an IPv4
address or hostname; IPv6 literals are not supported by the current client.
The computer and device need a working route to each other. The device's own
IP address is not the development-server address.

WI-FI NAME and WI-FI PASSWORD stage credentials. CHANGE WI-FI applies them.
An unchanged password keeps the current password. Editing the password to
empty explicitly selects an open network. The displayed password is always
masked. New credentials are saved in device NVS after connection succeeds;
a failed connection retries the previous Wi-Fi after 45 seconds. Changing
networks can change the device's IP and disconnect remote control.

Wi-Fi editing requires the updated PAX firmware. An older firmware displays
FIRMWARE UPDATE NEEDED. The virtual `/dev/pax-wifi` endpoint uses the existing
ABI-1 file services, returns only SSID/status/password-present, and does not
write credentials to the SD card. Firmware builds containing credentials
remain in the ignored `pax-dev/firmware-local/` folder.
