# Additional store packages

The store manifests cover AZIP, Recipe Installer, Wolfenstein 3D, Spear of
Destiny, Workspace (package ID `desktop`), Quake II, Scratch Blocks, Video
Player, the PAX SDK Test, OpenTyrian and Tiberian Sun.

Game data is supplied separately as described by each package's install recipe.
Tiberian Sun is a development runtime shell, not a complete playable game.
Quake II has substantial memory requirements. Scratch and Video retain their
existing development limitations; enabling a listing is not a new hardware
validation claim.

The public AZIP and Wolf4SDL `core` folders are source snapshots of the
workspace's symlink targets. `core/pax-source.json` records their origin.
Spear reuses Wolf4SDL sources with its own campaign flags and data directory.

Another World and Stargus also have manifests, but remain `publish: false`
and `requires_private_data: true`: their current native builds embed game data.
The default store build and developer prerelease selector skip these recipes.
They can be compiled explicitly in a complete local workspace:

```sh
python3 tools/build_pax.py anotherworld stargus --out /tmp/private-paxs
```

The `workspace` recipe delegates to the existing native build script and emits
a generated PAX JSON sidecar, preserving the binary header's actual entry offset.
It disables staging into the development SD image. Publication requires a
separate change to load all user-provided game assets from storage.

The committed Video Player release is enabled independently of any uncommitted
hardware-MJPEG experiment in the working tree.
