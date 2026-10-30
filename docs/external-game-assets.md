# Code-only Another World and Stargus packages

These PAX packages contain engine code only. User game graphics, resource banks
and encoded audio stay on SD. No firmware or numeric ABI change is required.
The source snapshots and generated Lua/Vorbis bindings contain code and metadata,
not converted game media. Stargus's sound catalog contains names and indices only.

In a standalone GitHub store checkout, the preparation tools are under `tools/`
instead of `pax-recipe/tools/`. The commands below use the workspace layout.

## Another World

Recipe Installer can read original DOS FAT12/16 floppy images. Place a ZIP
containing `.ima` or `.img` images, or individual images, in
`/sd/roms/anotherworld/incoming/`, open Another World in the launcher and
select its data installer. The installer reads root-directory `MEMLIST.BIN`
and `BANKxx` files, keeping existing destination files. Select each image when
the game spans separate files. ZIP extraction requires firmware with the
named ZIP extraction service; an individual image works without that service.
After copying, the installer checks the table and all referenced bank ranges.
This is not support for installer executables, FAT32, partitioned disks, or
files inside subdirectories on an image.

Alternatively, use the DOS data layout supported by the existing raw interpreter:

```sh
python3 pax-recipe/tools/prepare_anotherworld_data.py /path/to/original-dos-data /path/to/card/roms/anotherworld
```

This validates resource-table bounds and referenced bank ranges, and normalizes
filenames to `memlist.bin`, `bank01`, ..., `bank0d`. Do not substitute remastered
or incompatible game-edition files. To migrate the existing private local headers:

```sh
python3 pax-recipe/tools/prepare_anotherworld_data.py /path/to/private/core /path/to/card/roms/anotherworld --from-headers
```

The executable reads the small resource table at startup and opens only the bank
needed by each resource. Missing/truncated data stops loading cleanly. Resource
unpacking retains its CRC check and now rejects invalid run lengths and references.

## Stargus

Start with the complete tree made by the upstream Stargus extractor. Prepare its
private media on the computer before copying the tree to `/sd/roms/stargus/`:

```sh
python3 pax-recipe/tools/prepare_stargus_sd.py /path/to/converted-stargus
```

This produces `media/runtime.paxassets`, `media/menu.paxmenu`,
`media/terrain.paxtile` and `media/stargus_intro.ogg`. It uses the existing Pillow,
FFmpeg and ImageMagick conversion tools. Keep the converted scripts, graphics,
sounds and remaining files alongside `media/`.

Sprites are read on demand from `runtime.paxassets`. Each SDL surface owns its
loaded pixels and releases them through normal surface destruction. The intro
soundtrack streams from SD; menu and terrain companions retain their existing
storage paths. Startup checks the required companion headers before launching
the engine. Converted assets must never be committed or published with the PAX.

## Builds and verification

Both apps now use regular standalone `custom` store recipes. Per-group compiler
flags preserve the original engine/GUI/bridge RTTI and compatibility settings.
Stargus retains the global constructor and destructor tables in its linker script.
The old native build also removes obsolete embedded-media object files so an
incremental link cannot silently retain private data.

Host checks run without game data:

```sh
python3 -m unittest discover -s pax-modules/tests -p 'test_external_assets.py'
```

Optional private-data checks decode every DOS resource and compare every sprite
payload/palette against the prepared pack without copying assets into the tests:

```sh
PAX_ANOTHERWORLD_TEST_DATA=/path/to/dos-data PAX_STARGUS_TEST_PACK=/path/to/runtime.paxassets python3 -m unittest discover -s pax-modules/tests -p 'test_external_assets.py'
```
