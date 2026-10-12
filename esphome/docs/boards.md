# PAX three-button firmware boards

The three buttons are on-screen Storage / Network / GitHub Store. Touch and USB keyboard/gamepad navigation are included.

| Manufacturer | Board key | YAML | Status |
| --- | --- | --- | --- |
| Elecrow | elecrow | esp32-p4-elecrow-pax-3button.yaml | Current Elecrow AIO target, tested on our device |
| Waveshare | nano | esp32-p4-nano-pax-3button.yaml | NANO + ECA20-MHA1-I HDMI 720p60, Ethernet; hardware test pending |
| Waveshare | nano1080 | esp32-p4-nano-pax-hdmi1080.yaml | Same board/bridge at 1080p30; hardware test pending |
| Waveshare | waveshare7b | esp32-p4-waveshare-7b-pax-3button.yaml | Integrated ESP32-P4-WIFI6-Touch-LCD-7B, rev3; hardware untested |
| Waveshare | waveshare | esp32-p4-waveshare-pax-3button.yaml | ESP32-P4-WIFI6 + 7-inch 1024×600 DSI panel; hardware untested |

The Waveshare configuration follows the local legacy reference for panel timing and touch orientation. It is **not** a universal config for Waveshare 7B, 4B, XC, or newer P4 revisions. The ESP32-P4-WIFI6 schematic confirms 32 MB flash, SD CLK43/CMD44/D039/power45, ES8311 on I2C7/8 with MCLK13/BCLK12/LRCLK10/DOUT9 and amplifier53. Hosted C6 uses reset54, CLK18/CMD19/D014/D115/D216/D317. SD uses slot0 so hosted SDIO retains slot1. Elecrow ADC16 is omitted because it conflicts with SDIO.

Sources:
- https://docs.waveshare.com/ESP32-P4-WIFI6
- https://files.waveshare.com/wiki/ESP32-P4-WIFI6/ESP32-P4-WIFI6-datasheet.pdf
- Original local reference: esphomebrew/esphome/esp32-p4-waveshare-papp.yaml

For each additional board, create a hardware package under `boards/<manufacturer>/`, a thin entry point and an alias in `profiles.json`. See [shared configuration](configuration.md). Record exact product/revision, flash/PSRAM, display controller/timing/orientation, touch, SD power/bus/slot, audio codec/amp, Wi-Fi transport and USB wiring here. Preserve the current PAX components, API actions, three-button widgets and numeric ABI. Run config + compile before listing compile support; test display, touch, storage, Wi-Fi, USB input/audio on hardware before claiming device support. Board entries must state their validation level.

## NANO HDMI wiring and validation

NANO is 16 MB flash / 32 MB PSRAM. Ethernet IP101 uses MDC31, MDIO52, power51, external clock50 and PHY address1. SD uses CLK43/CMD44/D039/power45, slot1, one-bit mode. ES8311 audio and USB input use the NANO board connections; no touchscreen, panel reset or backlight GPIO is configured.

EBYTE ECA20-MHA1-I uses GSV1172, not LT8912B. The owner confirmed two-lane input support on 2026-10-11. Its firmware must accept two lanes, RGB888 and the chosen timing. The NANO and bridge connectors are not presumed pin-compatible: use a correctly mapped adapter, proper power and common ground. The video-only MIPI mode skips LCD DCS reset/initialization. 720p60 and 1080p30 use 74.25 MHz pixel clocks and 1000 Mbps per lane. 1080p60 is not provided. HDMI audio is not configured; audio goes through the NANO ES8311 output.

The PAX app canvas remains 1024×600, centered on the larger HDMI panel, to preserve module memory budgets and compatibility. USB pointer bounds cover the whole panel. NANO Wi-Fi is not enabled in these profiles; Ethernet DHCP and OTA are used. Monitor output, Ethernet PHY startup, storage, controls and audio require an actual hardware test.

References: https://docs.waveshare.com/ESP32-P4-NANO and https://files.waveshare.com/wiki/ESP32-P4-NANO/ESP32-P4-NANO-schematic.pdf and https://www.cdebyte.com/products/ECA20-MHA1-I/7

The Elecrow, NANO and original WIFI6 packaged images use `engineering_sample: true`, targeting ESP32-P4 revisions 0/1 (image maximum revision 199). Production rev3 requires `engineering_sample: false` and a separate compilation; do not use these prebuilt images on rev3. Check the chip revision in the boot log.

## Waveshare Touch-LCD-7B

This is the integrated 7B unit confirmed by the owner, distinct from the earlier WIFI6 + separate panel reference. EK79007 uses 52 MHz pixel clock, H sync10/back160/front160 and V sync1/back23/front12. LCD reset33, inverted backlight32 at5kHz, GT911 on I2C7/8 in polling mode with no dedicated reset/interrupt pins and mirror X/Y, display rotation0. Audio uses ES8311 MCLK13/BCLK12/LRCLK10/DOUT9 and amplifier53. SD uses CLK43/CMD44/D039, GPIO45 power switch and slot0; C6 hosted SDIO retains slot1. LDO3 powers DSI at2.5V; LDO4 uses passthrough (3.3V supply) for SD rather than using UHS voltage switching. No camera, microphone echo cancellation or RTC feature is claimed by this launcher profile.

The 7B profile uses production rev3 silicon (`engineering_sample: false`) as in the current official examples. The other profiles target early rev0/1. Match your actual chip revision before flashing.

Primary references: https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-7B and https://github.com/waveshareteam/Waveshare-ESP32-components/tree/master/bsp/esp32_p4_wifi6_touch_lcd_7b and https://github.com/espressif/esp-iot-solution/tree/master/components/display/lcd/esp_lcd_ek79007

## Additional experimental profiles

All active profiles include the central PAX base. New profiles have configuration validation and require external hardware testing. Their full compile results are recorded in the workspace validation report.

| Manufacturer | Build key | Hardware combination | Main differences |
| --- | --- | --- | --- |
| Waveshare | nano-wifi | NANO + ECA20-MHA1-I HDMI 720p60, C6 Wi-Fi | SD slot0, hosted SDIO slot1, LDO4 passthrough; existing Ethernet aliases retain their settings |
| Waveshare | eth-hdmi720 | ESP32-P4-ETH + ECA20-MHA1-I HDMI 720p60 | IP101 Ethernet, 32MB flash per vendor, SD slot1, ES8311 audio |
| Waveshare | waveshare4b | WIFI6-Touch-LCD-4B, 720x720 | ST7703 panel model, LCD reset27, inverted PWM26 and enable33, touch reset23, SD power45 |
| M5Stack | tab5 | Original Tab5 ILI9881C + GT911, 720x1280 | C6 SDIO12/13/11/10/9/8/reset15; I2C31/32; expander display/touch reset and USB/Wi-Fi/amp power; ES8388 LINE1 |
| Guition | jc1060 | JC1060P470, 1024x600 | Panel reset5 and H-sync20, GT911 reset22/interrupt21, PWM23, amp20, ES8311 |
| Guition | jc4880 | JC4880P443C_I_W, 480x800 | ST7701, panel reset5, GT911 reset3 in polling mode, PWM23, amp11, SD LDO4 passthrough with no GPIO45 power control |
| Guition | jc8012 | JC8012P4A1, 800x1280 | JD9365, panel reset27, GSL3680 reset22/interrupt21 and mirror Y, PWM23, amp20, pinned touch driver |
| Guition | m3-hdmi720 | ESP32-P4-M3-Dev + ECA20-MHA1-I HDMI 720p60 | C6 SDIO, USB Serial/JTAG, amp11, ES8311, SD LDO4 passthrough; GPIO45 power resistor is NC in the reference |

These new profiles target early rev0/1 silicon (`engineering_sample: true`), following the supplied panel examples and original board revisions. Production rev3 needs a separately compiled override. Tab5 ST7123/ST7121 revisions are not covered by the original Tab5 profile. Bare ETH and M3 boards have no implied default LCD: the provided combinations explicitly select the HDMI module already used by our NANO setup, and need their own correctly wired adapters. HDMI sound is not configured.

Guition SD uses CLK43/CMD44/D039, slot0 while C6 uses slot1. JC8012 and JC1060 schematics route the amplifier to GPIO20; the M3/JC4880 module uses GPIO11. All omit microphones, cameras, batteries, relays, Bluetooth and unrelated sensors. The 480/720-pixel profiles use smaller native canvases; applications that rely on a fixed 800x480 or 1024x600 display need individual testing.

The supplied 4B example's hosted SDIO pins match the existing Waveshare C6 interface and are retained as an experimental assumption. The current vendor page has a conflicting wireless signal summary, although its BSP agrees with the audio, LCD and SD wiring used here. Confirm the C6 pin map against the exact PCB schematic before hardware deployment. No 4B connectivity claim is made from compilation alone.

Source references and immutable Git revisions: [board-sources.json](../board-sources.json). Starting points are the [ESPHome device pages](https://devices.esphome.io/), [jtenniswood hardware examples](https://github.com/jtenniswood/esphome-lvgl), [Waveshare BSP](https://github.com/waveshareteam/Waveshare-ESP32-components), [M5Stack Tab5 BSP](https://github.com/m5stack/M5Tab5-UserDemo), [Guition schematic archive](https://github.com/p1ngb4ck/unofficial_guition_esp32p4_repo), and [JC4880 BSP pin definitions](https://github.com/ultramcu/guition-jc4880p4-bsp).

## Espressif, Olimex and DFRobot

| Manufacturer | Build key | Hardware combination | Scope |
| --- | --- | --- | --- |
| Espressif | espressif-ev | Function EV v1.5.2 with supplied 7-inch EK79007/GT911, Wi-Fi, ES8311 | Early P4 silicon; LCD adapter reset27/PWM26 jumpers; active-low SD power45 |
| Olimex | olimex | ESP32-P4-DevKit rev D1 + ECA20-MHA1-I HDMI720p60, Ethernet | IP101 address1, no onboard Wi-Fi or audio; active-low SD power45; USB host from expansion header |
| DFRobot | firebeetle2 | FireBeetle2 ESP32-P4 DFR1172 + ECA20-MHA1-I HDMI720p60, Wi-Fi | No speaker output; PDM microphone omitted; active-low SD power45; powered USB hub |

See [manufacturer pinouts](pinouts.md) for signals, source schematics, connector scope and silicon revisions. These combinations are experimental and require hardware testing. Bare-board DSI outputs do not imply support for every compatible FPC display; select exact panel or bridge timings.
