# SD-card extraction recipes (version 1)

Launcher recipes are declarative JSON data attached to an app's information
JSON. They are not executable scripts: the launcher accepts only the media
formats and copy operations implemented by its recipe version. A recipe can
read a ZIP member, locate listed files in its ISO9660 data track, and copy only
those files to game-data paths. It cannot run commands or write outside
`/sd/roms/` or `/usb0/roms/`.

## Example

Add this top-level object to `redalert.json` (alongside `name`, `requires`,
etc.). Replace the ISO/CUE member name and ISO paths with the names on the
actual CD. The example file names are illustrative, not a claim about a
particular Red Alert release's disc layout.

```json
{
  "extract": {
    "version": 1,
    "archive_dir": "/sd/roms/redalert/incoming",
    "media": [
      {
        "format": "zip-iso9660",
        "zip_name": "redalert-disc-1.zip",
        "entry": "CD/REDALERT.ISO",
        "files": [
          {
            "source": "INSTALL/REDALERT.MIX;1",
            "target": "/sd/roms/redalert/REDALERT.MIX"
          },
          {
            "source": "INSTALL/ALLIED/MAIN.MIX;1",
            "target": "/sd/roms/redalert/allied/MAIN.MIX"
          }
        ]
      },
      {
        "format": "zip-cue-bin",
        "zip_name": "redalert-disc-2.zip",
        "entry": "disc/REDALERT.CUE",
        "files": [
          {
            "source": "SOVIET/MAIN.MIX;1",
            "target": "/sd/roms/redalert/soviet/MAIN.MIX"
          }
        ]
      }
    ]
  }
}
```

`archive_dir` is where the user places the source ZIP files. If `zip_name` is
omitted, each media recipe checks ZIP files in that folder until it finds one
containing the exact `entry` member. `entry` is the exact ZIP member path to
the ISO or CUE file. For CUE, the referenced BIN must be in the same ZIP; its
path is resolved relative to the CUE member's folder.

Each `source` is a path inside the disc data track. Each `target` is the final
required game-data path. The target is also added to the info page's file
checker when it is not already declared in `requires`/`data.files`. Optional
files may be marked with `"optional": true`.

## Supported media and safety behavior

- `zip-iso9660`: standard single-volume ISO9660 primary-volume images.
- `zip-cue-bin`: CUE sheets using `FILE ... BINARY` and the first Mode 1 data
  track (`MODE1/2048` or raw `MODE1/2352`, with an `INDEX 01`). The ISO9660
  primary-volume filesystem in that track is read.
- ZIP members may be stored or deflated, must not be encrypted, and are capped
  at 1 GiB per staged disc image. ZIP64 and split archives are not supported.
- The complete ISO/BIN member is temporarily staged in the recipe folder, so
  the card needs free space roughly equal to the uncompressed disc image. The
  temporary image is removed after processing.
- Only recipe-listed source files are copied. Existing target files are never
  overwritten. New files are written to a temporary sibling and renamed into
  place only after a complete successful copy; a failed copy removes its
  temporary output.
- The reader currently supports the ISO9660 primary volume and ordinary
  ASCII file names (including `;1` version suffixes). Joliet/Rock Ridge,
  multi-extent ISO files, and unusual CUE track layouts are not supported.

Use one `media` item per disc image when a game needs several CDs. Give each a
distinct `zip_name` when their image member names repeat. After adding or
changing a recipe, reopen the app's information page so its JSON is re-read;
the **EXTRACT DATA** action appears only when the recipe validates. The action
updates the file check list and reports any missing mappings or unsupported
disc layout in the page status.
