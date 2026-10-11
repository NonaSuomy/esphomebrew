# ESPHomeBrew
ESPHomeBrew is a GitHub-hosted store of PSRAM apps (`.pax`) for the ESP32-P4.

- `apps/<name>/pax.json` describes each app; GitHub Actions builds them into `.pax` files. See [docs/building.md](docs/building.md).
- ESP32-P4 devices running ESPHome with the `pax_loader` component browse the store from an LVGL page and stream apps into PSRAM.

## 💖 Support the Project

ESPHomeBrew is developed in my spare time for fun! If you find it useful and
want to support continued development, you can support the project on
[Ko-fi](https://ko-fi.com/nonasuomy).

No pressure, though, trying ESPHomeBrew and sharing feedback is already
incredibly valuable!

PAX app sources are organized under `apps/<appname>/`. Older alternate emulator sources live in each app’s `variants/legacy/` folder. Recipes select their explicit pinned upstream inputs and local adapters in `ports/`; see `MODULES.md`, `modules.json` and `app-source-migration.json` for the source inventory. Local build checks do not establish that GitHub Actions have passed in a newly created repository.


Active recipe adapters and copied cores are in `apps/`. `legacy/` contains preserved reference snapshots and is excluded from active builds. This repository is the standalone publishing/build snapshot; native engine Git checkouts remain in the full PAX workspace and are exposed there through app dependency links. See `docs/UPSTREAM-UPDATES.md` for provenance and updating bases.

## Browser firmware installer

`web-installer/` is published to https://nonasuomy.github.io/esphomebrew/install/ by the existing Publish store workflow. Device cards are grouped by manufacturer in `boards.json`; NANO and Touch-LCD-7B are both Waveshare. Keep exact display and silicon-revision choices visible. Firmware is compiled locally from the patched PAX workspace, checked by `pax-tools/stage-web-installer.py`, then verified again by `tools/check_web_installer.py` during Pages deployment. Factory images and flash kits are public credential-free builds; never copy `firmware-local` into this folder. Hardware status must only be changed after actual device tests.
