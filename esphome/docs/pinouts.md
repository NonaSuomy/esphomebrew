# Additional ESP32-P4 board pinouts

These pinouts are derived from the manufacturers' documentation and schematics. The active PAX profiles use the shared `packages/pax-base.yaml`; board wiring stays in the manufacturer package. None of these three boards has been tested on our hardware.

## Espressif ESP32-P4-Function-EV-Board

Profile: `espressif-ev`, `boards/espressif/function-ev-7inch.yaml`. Scope: hardware v1.5.2, early P4 rev0/1, 16MB flash and 32MB PSRAM, supplied 7-inch EK79007/GT911 panel. P4X Function EV with rev3 silicon is a different target.

| Peripheral | Signals |
| --- | --- |
| C6 hosted Wi-Fi | RESET54, CLK18, CMD19, D0=14, D1=15, D2=16, D3=17; SDIO slot1 |
| Shared I2C | SDA7, SCL8, 400kHz; ES8311 and GT911 |
| Display | Two native DSI lanes; LDO3 at2.5V; LCD adapter RST_LCD jumper to GPIO27 and PWM jumper to GPIO26 |
| EK79007 timing | 1024x600, 52MHz, H sync10/back160/front160, V sync1/back23/front12; 900Mbps/lane |
| Touch | GT911, address0x5D, polling; mirror X/Y as in the BSP; no dedicated reset/interrupt GPIO |
| SD | CLK43, CMD44, D0=39; available D1=40/D2=41/D3=42; slot0 in one-bit mode; active-low power45; LDO4 passthrough |
| Audio output | ES8311 address0x18; MCLK13, BCLK12, LRCLK10, DOUT9; amplifier53 |
| Audio input | DIN11; not configured in this launcher profile |
| Programming | USB Serial/JTAG port; do not use the high-speed host Type-A port for flashing |
| USB inputs | High-speed USB Type-A, distinct from the programming port |

Use the supplied LCD adapter and the reverse-direction display FPC cable specified by Espressif. The two reset/PWM jumper wires are required. Power the LCD adapter as documented by the manufacturer.

Sources: [hardware guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32p4/esp32-p4-function-ev-board/user_guide.html), [official BSP header](https://github.com/espressif/esp-bsp/blob/f259a6abd5808019b1903a422092a4234ca95a51/bsp/esp32_p4_function_ev_board/include/bsp/esp32_p4_function_ev_board.h), and [EK79007 timing definitions](https://github.com/espressif/esp-iot-solution/blob/master/components/display/lcd/esp_lcd_ek79007/include/esp_lcd_ek79007.h).

## Olimex ESP32-P4-DevKit

Profile: `olimex`, `boards/olimex/devkit-hdmi720.yaml`. Pin reference: PCB rev D1, 16MB flash and 32MB PSRAM. The provided firmware combination uses ECA20-MHA1-I HDMI at720p60 and targets early P4 silicon. It is not a profile for Olimex's LT8912-based MIPI-HDMI adapter.

| Peripheral | Signals |
| --- | --- |
| Ethernet | IP101, PHY address1; MDC31, MDIO52, reset51, external RMII clock50 |
| SD | CLK43, CMD44, D0=39; available D1=40/D2=41/D3=42; slot1, one-bit mode; active-low PMOS gate45; LDO4 passthrough |
| SD card detect | GPIO3; pin recorded, not configured |
| Display | Native two-lane DSI, LDO3 at2.5V; board FPC follows Raspberry Pi1/2/3/4 style |
| I2C header/display | SDA7, SCL8; reserved in pin reference, no touch controller assumed |
| UEXT | UART TX37/RX38; SPI MOSI52/MISO54/SCLK4/CS5; I2C7/8 |
| Onboard LED | GPIO2; not configured |
| Programming | Type-C USB Serial/JTAG on USB1; logger uses USB_SERIAL_JTAG |
| USB host | Dedicated high-speed USB DP/DN on expansion header, with a suitable external host connector and power supply |
| Wi-Fi/audio | No onboard Wi-Fi coprocessor or audio output codec; these are omitted |

UEXT MOSI52 shares the Ethernet MDIO GPIO, so do not enable that SPI connection while using Ethernet. The HDMI combination needs the two-lane ECA20-MHA1-I bridge and a correctly mapped, powered adapter. The board's DSI FPC must not be assumed interchangeable with the bridge connector. To use the Olimex MIPI-HDMI accessory, add its LT8912 I2C initialization and exact vendor timing as a separate display package.

Sources: [product and pinout resources](https://www.olimex.com/Products/IoT/ESP32-P4/ESP32-P4-DevKit/open-source-hardware), [rev D1 schematic](https://github.com/OLIMEX/ESP32-P4-DevKit/blob/26705d36407a07324348927dfd30fbf4ffc1d94c/HARDWARE/ESP32-P4-DevKit-Rev.D1/ESP32-P4-DevKit_Rev_D1.pdf), and [vendor Ethernet configuration](https://github.com/OLIMEX/ESP32-P4-DevKit/blob/26705d36407a07324348927dfd30fbf4ffc1d94c/SOFTWARE/Demo_Examples/iperf/sdkconfig).

## DFRobot FireBeetle 2 ESP32-P4

Profile: `firebeetle2`, `boards/dfrobot/firebeetle2-hdmi720.yaml`. Exact model: DFR1172, schematic V1.0.0, 16MB flash and 32MB PSRAM. FireBeetle 2 C5, C6, S3 and ESP32-E variants are not interchangeable with this P4 profile. The provided combination uses ECA20-MHA1-I HDMI at720p60 and early P4 silicon.

| Peripheral | Signals |
| --- | --- |
| C6 hosted Wi-Fi | RESET54, CLK18, CMD19, D0=14, D1=15, D2=16, D3=17; SDIO slot1 |
| C6 wakeup | GPIO6; pin recorded, not configured |
| SD | CLK43, CMD44, D0=39; available D1=40/D2=41/D3=42; slot0, one-bit mode; active-low SD1_PWRN45; LDO4 passthrough |
| Display | Two native DSI lanes, LDO3 at2.5V; Raspberry Pi-style FPC requiring the correct HDMI adapter mapping |
| I2C | SDA7, SCL8; exposed to DSI/CSI connectors and headers |
| PDM microphone | CLK12, DATA9; input only, omitted from this launcher profile |
| Onboard LED | GPIO3; not configured |
| Boot button | GPIO35 |
| UART | TX37, RX38 |
| Programming | USB Serial/JTAG Type-C, distinct from high-speed OTG Type-C |
| USB inputs | High-speed OTG Type-C with a suitable host adapter and externally powered hub |
| Audio output | No onboard speaker codec or HDMI audio configured |

The manufacturer's pin table labels GPIO45 as EN. Its actual schematic connects GPIO45 to a P-channel MOSFET gate named SD1_PWRN, so the PAX power pin is inverted and drives low to power the SD card. The microphone pins are not reused as an imaginary speaker bus.

Sources: [DFR1172 documentation and pin table](https://wiki.dfrobot.com/dfr1172/), [official V1.0 schematic](https://dfimg.dfrobot.com/wiki/21103/DFR1172_firebeetle-esp32-p4r32-development-board_schematics_V1.0.pdf), pages1/3/4 for GPIO, USB and SD power.

Immutable Git revisions and source links are also recorded in [board-sources.json](../board-sources.json). Use [configuration.md](configuration.md) to add another display without duplicating PAX services.
