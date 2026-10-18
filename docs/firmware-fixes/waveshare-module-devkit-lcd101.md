# Waveshare ESP32-P4-Module-DEV-KIT with 10.1-inch display

The installer adds a separate Waveshare Module Dev Kit option for the supplied
`101-tab-dev.yaml` JD9365/GT911 display configuration. Select the exact board and
**10.1-inch, 1280 x 800, Ethernet + Wi-Fi** profile. This is experimental firmware;
compilation and automated validation do not establish hardware compatibility.

The image targets early P4 revision 0/1 at 360 MHz with 16 MB DIO flash. Revision 3
requires a separate build. Connect its USB-to-UART programming port for serial
installation and Wi-Fi provisioning. Follow the vendor USB host jumper settings
when connecting USB input devices.

- Display: native 800x1280 JD9365 at 270 degrees, GT911 with swapped axes and
  mirrored X; I2C GPIO7/8, panel power/backlight controller 0x45.
- Networking: IP101 Ethernet DHCP preferred, with ESP32-C6 SDIO Wi-Fi fallback.
  C6 reset54, CLK18/CMD19/D0=14/D1=15/D2=16/D3=17, SDMMC slot 1.
- SD: four-bit GPIO43/44/39/40/41/42 on slot 0, LDO4 at 2.7 V, active-low GPIO45
  power control as shown in the official schematic.
- Speaker: ES8311 at 0x18, MCLK13/BCLK12/LRCLK10/DOUT9, amplifier enable53,
  existing PAX 48 kHz mixer/resampler chain. Microphone and camera are not enabled.
- Setup: `http://DEVICE_IP:8080/pax` or `http://DEVICE_NAME.local:8080/pax`.

Existing board profiles retain their resolved configurations and public firmware
images. The new flash kit includes the board/display YAMLs, shared packages,
landscape pixel adapter, setup component source, checksums and SD starter.
The supplied YAML's private account settings and demonstration services are
excluded. No saved Wi-Fi credentials, API encryption key or OTA password are
included in the public image.

Automated checks cover board pins, display/touch transforms, SDIO slot separation,
button bounds, preservation of existing resolved profiles, firmware hashes and
flash-kit contents. Validate display, all four touch corners, brightness, SD
writes, Ethernet/Wi-Fi failover, USB input and speaker on the physical unit before
marking it hardware tested.

Sources: supplied `101-tab-dev.yaml` and board photo;
[official documentation](https://docs.waveshare.com/ESP32-P4-Module-DEV-KIT) and
[schematic](https://files.waveshare.com/wiki/ESP32-P4-Module-DEV-KIT/ESP32-P4-Module-DEV-KIT.pdf).
