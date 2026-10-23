# Device firmware builds

**Build device firmware** compiles the shared three-button firmware for every public profile in `esphome/installer-profiles.json`. The profile keys must exactly match the devices advertised in `web-installer/boards.json`. Private configurations and desktop builds are excluded.

The workflow runs on firmware source changes, pull requests, and **Actions → Build device firmware → Run workflow**. Eight board jobs can run concurrently. A failure leaves the other jobs running so the result identifies every failing profile.

## Reproducible inputs

`tools/ci/firmware-sources.json` pins public ESPHome and ESP-IDF base commits and SHA-256-checked patches. These reconstruct the runtime sources used by the locally tested builds, including DSI, audio, USB, FATFS, and Wi-Fi compatibility fixes. `requirements-firmware.txt` pins host Python dependencies. Source and PlatformIO tool caches avoid repeated downloads. No station credentials or local toolchain directories are distributed.

The generated matrix validates every local YAML include before compilation. Any `!secret` or saved station credentials in the include closure is rejected. Profiles keep their existing silicon revision, clock, display, touch, audio, network, and SD settings.

## Outputs

Each successful board job uploads factory and OTA images, an offline flash kit with the SD starter and source references, a flash manifest, checksums, and per-profile build metadata. Logs are uploaded even if compilation fails. The factory image is for initial flashing; the OTA image requires an existing compatible installation.

The assembly job accepts only a complete matrix with matching source fingerprints and intact factory/OTA hashes. It checks the existing installer before uploading the combined `firmware-installer` artifact. A successful compile is not a physical hardware test; earlier hardware evidence is retained separately instead of attached to a new image as a fresh test.

## Automatic publication

Only a successful complete matrix on `main` can publish. The publication job checks out current `main` and compares the firmware input fingerprint before copying the verified artifacts into the installer. Changed inputs, missing profiles, hash failures, or a concurrent Git push stop publication. Pull requests produce artifacts but cannot update the installer.

The bot commits the complete set of installer binaries and metadata, then explicitly dispatches the existing **Publish store** workflow. This dispatch is required because a push using `GITHUB_TOKEN` does not start other push workflows. App versions and game data are unchanged by the firmware publication step.

The workflow requests `contents: write` and `actions: write` only in the publication job. A branch protection rule that prohibits bot pushes must permit this job or the final commit will fail while the verified artifacts remain available.

## Local checks

```
python -m pip install PyYAML
python -m unittest discover -s tests -p test_firmware_ci.py -v
python tools/firmware_ci.py matrix
```

To reproduce a board build on a clean Linux checkout:

```
python -m pip install -r tools/ci/requirements-firmware.txt
python tools/firmware_ci.py bootstrap
python tools/firmware_ci.py build nano-lcd43
```

`bootstrap` fetches the pinned source dependencies. It refuses an incompatible existing dependency cache instead of resetting local changes. Outputs go to `firmware-dist/<profile>/`.
