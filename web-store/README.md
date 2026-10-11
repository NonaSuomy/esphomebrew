# Browser app installation

`store-install.js` provides the shared catalog verifier, download checks and USB serial transport. The catalog generator loads it on the public store. `pax-tools/generate-web-assets.py` embeds the same source in the device's Wi-Fi setup page.

USB uses vendor commands in the existing Improv serial frame (115200 baud). Requests have type 3 and payload `command | binary data`; responses have type 4 and payload `command | status | UTF-8 message`. Both use the existing frame checksum. Status 0 means accepted or verified, 1 means failure, and 2 means busy. Commands are 0x70 begin, 0x71 chunk, 0x72 completion status, 0x73 cancel, 0x74 capability probe (`PAXSD1`), and 0x75 close app. Standard Improv provisioning commands retain their original behavior.

Begin data is little-endian uint32 size, 32 raw SHA-256 bytes and the UTF-8 `/sd/` path. Chunks carry little-endian uint32 sequence followed by up to 240 bytes. A bounded queue feeds the loader's existing staged file writer. USB selection grants access to the physically connected device. Wi-Fi mutations continue to require the device's own origin and session token. Public store selections arrive as URL fragments and require confirmation on the device page before anything is written.

Run `node pax-modules/tests/test_store_install.cjs` for packet corruption, split-frame handling, app metadata/hash validation and transfer completion tests. Run `python pax-esphome/tests/check_web_storage.py` to test the actual storage writer's integrity and rollback behavior. Hardware tests must separately verify the firmware and serial wiring for each board.
