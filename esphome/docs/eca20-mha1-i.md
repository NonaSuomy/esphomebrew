# EBYTE ECA20-MHA1-I HDMI profile

The PAX external `mipi_dsi` component includes the `ECA20-MHA1-I` model based on EBYTE's supplied `hdmi_out.c` example. The source hashes are recorded in `eca20-vendor-source.json`. The implementation was adapted from ESPHome commit a01d06a47, preserving PAX's ESP-IDF 6.1 RGB565 to RGB888 conversion and framebuffer handoff support.

`displays/hdmi720.yaml` selects the named model: two lanes at 1000 Mbps each, 1280 x 720 at 60 Hz, a 74.25 MHz pixel clock, H front/sync/back 110/40/220 and V front/sync/back 5/5/20. PAX retains its 16-bit framebuffer and sends 24-bit RGB to the bridge. The generic model defaults to a 24-bit framebuffer when used outside this package. Both HDMI packages enable continuous HS clock after panel initialization and skip LCD command traffic. The existing 1080p30 package remains a CUSTOM timing profile and requires separate confirmation from EBYTE.

The component is tracked in the nested ESPHome dependency repository. `patches/0003-eca20-mipi-dsi.patch` preserves the changes against the dependency base recorded in the source manifest; apply it at that repository root only when restoring the matching older checkout. It is already applied in this workspace.

Build from the PAX workspace:

```sh
./pax-tools/build-firmware.sh compile nano
```

Other boards that include `displays/hdmi720.yaml` share the profile. Board wiring stays in `boards/`. No vendor GPIO or LDO4 defaults were copied: GPIO1, GPIO48 and the vendor's 1.8 V rail are specific to their example board. Set a reset or power pin only for verified wiring. When a reset pin is configured, bridge reset is low for 100 ms followed by a 200 ms boot wait before DSI starts. LDO3 remains the board's DSI PHY supply, not the module's 5 V supply.

The matching two-lane bridge firmware is still pending from EBYTE. These configurations are experimental and have not been tested with an HDMI module or monitor. The supplied example configures the P4 transmitter only. It does not provide GSV1172 register initialization, bridge firmware flashing, EDID, automatic monitor selection or audio. Vendor files remain in the original supplied directory; no license for redistributing those files was supplied.
