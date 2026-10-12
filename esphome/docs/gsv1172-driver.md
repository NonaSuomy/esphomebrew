# GSV1172 ESP-IDF driver investigation

Reviewed 2026-10-12. This is a source-based design assessment, not an implemented or hardware-tested GSV1172 driver. The ECA20-MHA1-I module and the bare GSV1172 chip must be treated separately: the module has its own programmed MCU/flash, power circuitry and optional resistor connections.

## What the references provide

Espressif's `esp_lcd_lt8912b` is a useful model for an ESP-IDF panel component. It creates an `esp_lcd_new_panel_dpi()` framebuffer panel, retains its original init/delete callbacks and wraps panel operations with the bridge's reset and I2C control. It also provides timing structures and an HDMI-ready query. Its chip-specific register tables, three I2C addresses, PLL programming, HPD bits and AVI packet handling are not GSV1172-compatible.

The pinned Olimex LVGL demo calls `bsp_display_start()` rather than implementing a bridge itself. Its sdkconfig selects HDMI, RGB888 and 1280x720 at60Hz through the Function EV BSP. It is a reference for the BSP/LVGL integration layer and display test application, not a GSV1172 initialization sequence.

Sources reviewed:

- [Espressif bridge component](https://github.com/espressif/esp-bsp/tree/f259a6abd5808019b1903a422092a4234ca95a51/components/lcd/esp_lcd_lt8912b), Apache-2.0, component version0.2.1.
- [Olimex display demo](https://github.com/OLIMEX/ESP32-P4-DevKit/tree/26705d36407a07324348927dfd30fbf4ffc1d94c/SOFTWARE/Demo_Examples/display_lvgl_demos).
- User-provided `ECA20-MH_ProductManual_EN_V1.1.pdf`, module manual.
- User-provided `GSV1172_v0.10.pdf`, preliminary chip specification. A manufacturer-hosted copy is [available here](https://www.ebyte.com/Uploadfiles/Files/2025-10-13/202510131533262294.pdf).

The manuals' text and relevant pinout/I2C/reset diagrams were inspected. The supplied PDFs are not redistributed in this repository; their SHA-256 values are recorded in `gsv1172-driver-sources.json`.

## Verified hardware and control facts

| Item | Evidence and implication |
| --- | --- |
| Native chip input | GSV1172 section1.2.3 lists 1/2/3/4-lane D-PHY, burst and non-burst modes, and RGB888/RGB666/RGB565 plus other formats. Chip capability does not establish which formats the installed module firmware accepts. |
| Module firmware | EBYTE sections1.2 and2.6 describe four-lane input and factory 1920x1080 at60Hz. The owner previously confirmed two-lane support for their module. Keep that confirmation separate from the manual's stock configuration. |
| I2C address | GSV1172 section3.1.2 gives 0xB0 as an eight-bit address. Its corresponding ESP-IDF seven-bit address is 0x58, inferred by removing the R/W bit. A probe ACK alone is not verified chip identity. |
| I2C transactions | The specification describes a page/device address, 16-bit register address and eight-bit register value. Reads use a repeated START. Confirm byte order and any additional bank/page addressing with the programming guide before implementing register access. |
| Bus speed | Section3.2.3 specifies a maximum I2C SCL rate of400kHz. Use a conservative probe speed initially, with correct pull-ups and the actual module wiring. |
| Module I2C wiring | EBYTE section2.2: input FPC pin22 is SDA and pin23 is SCL, at3.3V. R11 and R12 must be fitted for those connections to be usable. Do not expect a stock module to answer just because the FPC exposes those labels. |
| Module reset | The module normally uses an RC reset circuit. FPC pin21 can provide external reset only with R4 fitted. The chip's RESETB is active low. Its section3.1.1 timing figure shows a low pulse longer than1ms after the supply sequence. Boot/MCU readiness after release still needs vendor guidance. |
| Module power | FPC pins25/26 are5V inputs. The module handles the chip's internal supplies; ESP32-P4 DSI LDO3 supplies its own PHY, not the module's5V input. |
| HDMI signals | The module HDMI table labels DDC at5V and HPD at1.8V. These are different from its3.3V control I2C. Do not directly connect these signals to P4 GPIO without verifying the circuitry and level translation. |
| Internal MCU | GSV1172 section1.2.7 lists internal/external MCU operation and a mailbox. It does not define the mailbox commands or how an external host safely takes over a factory-programmed module. |

## Relationship to the current PAX profiles

`displays/hdmi720.yaml` and `displays/hdmi1080.yaml` configure the ESP32-P4 DSI/DPI transmitter only. They use two lanes, RGB888 on the wire, a74.25MHz pixel clock and1000Mbps per lane, with no DCS initialization sequence. This matches the module manual's distinction between an already-configured bridge and an ordinary LCD needing DCS commands, but only when the bridge firmware accepts the selected timing and format.

The manual's example is148.5MHz, 1920x1080 at60Hz, four lanes. Its explicit device-tree example uses H front88/sync44/back148 and V front4/sync5/back36. The preceding English table swaps the front/back prose labels; use the explicit device-tree field names when comparing timings. Some Allwinner values in the same manual are inconsistent, so they must not be copied directly into ESP-IDF.

For the current RGB888 format, stock1080p60 implies148.5MHz x24bits /2lanes =1782Mbps per lane before additional protocol constraints. Our locally pinned IDF6.1 P4 HAL limits the PHY to1500Mbps and two lanes. Consequently, copying the stock four-lane1080p60 recipe into a two-lane RGB888 profile is not appropriate. The existing720p60/1080p30 profiles stay experimental and require matching bridge firmware; no new mode or flash was made during this investigation.

The chip also lists RGB565 input, which changes the bandwidth calculation. An alternative1080p60 design using a different wire format would require confirmed module firmware support, complete DSI scheduling/bandwidth validation and physical testing. RGB565 framebuffer storage in the current profiles does not mean RGB565 transmission: their DPI path expands to RGB888 on the wire.

## Proposed component design

Use an ESP-IDF component called `esp_lcd_gsv1172` with these layers:

1. Board configuration: optional connected reset GPIO, control I2C handle, module/firmware identification and power sequencing assumptions.
2. GSV transport: documented address/register framing, repeated-start reads, finite timeouts and bus error propagation. Do not copy LT8912B's eight-bit register framing or address banks.
3. Vendor control backend: factory-firmware mailbox or validated external-host register sequence. Keep these as distinct modes. Factory mode must not silently write a guessed initialization table or interfere with the embedded MCU.
4. Display configuration: validated lane count, format, pixel clock and complete porch/sync timings. Reject unsupported combinations before allocating framebuffer resources.
5. Panel integration: `esp_lcd_new_panel_gsv1172()` creates/wraps the DPI panel, using the Espressif callback pattern while preserving the underlying panel's ownership and callbacks. Register verification and HPD readiness need separate evidence from successful DPI initialization.
6. Runtime services: known HPD/video-lock status, EDID retrieval and optional audio, each implemented only from documented registers or mailbox commands. Monitor disconnection and reconnect must have bounded recovery.

For a factory-configured module, a small DPI wrapper without bridge register writes may be enough to display video. A complete configurable GSV1172 driver additionally requires the vendor information below. Do not label a transport-only or passthrough component as a validated HDMI control driver.

## Information needed from EBYTE or GSCOOLINK

- GSV1172 programming guide/register map and SDK or a complete initialization example, with a usable source license.
- Chip-ID/revision registers and the full page/bank scheme, register byte order, safe read-only status fields and reserved-register rules.
- Internal-MCU/mailbox command API, host-control ownership/arbitration, boot-ready indications and persistent configuration/flash procedures.
- Two-lane ECA20-MHA1-I firmware/profile details for720p60 and1080p30: RGB format, burst/non-burst mode, clock continuity, DSI packet expectations, lane mapping/polarity and exact timings.
- PLL/DSI receiver/HDMI transmitter initialization and recovery sequence, required delays, HPD/video-lock reporting, EDID/DDC access and optional I2S audio configuration.
- Board schematic/BOM for the exact revision, including R4/R11/R12 values, fitted state, pull-ups and signal-level translation.

The supplied preliminary specification describes the transport and features but does not contain these functional register definitions. The public references found during this review do not supply a verified GSV1172 control SDK. No initialization addresses or values have been invented.

## Validation sequence when the hardware and SDK are available

Start with a standalone IDF test card, independent of LVGL and the PAX launcher. Verify power, common ground, correct two-lane FPC adapter mapping and the installed bridge firmware first. Probe only the documented control connection, then verify a documented chip-ID register. Test factory-mode video before any host takeover.

Unit-test transport byte ordering, repeated START, errors/timeouts and cleanup with a fake bus. Compile the component and example against the pinned P4 IDF. Use a known register trace to verify reset/init ordering. On hardware, test720p60 and1080p30, RGB color bars, HPD disconnect/reconnect, several monitors, cold/warm reset and long-running scanout. Add LVGL and PAX only after that path is stable. Audio is a separate follow-up.
