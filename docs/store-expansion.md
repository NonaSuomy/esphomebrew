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

Another World and Stargus use standalone code-only recipes as of 0.2.0.
Their game assets are supplied separately from SD. See
[external-game-assets.md](external-game-assets.md) for preparation commands.
The legacy private-data build guard remains available for future workspace-only
recipes, but these two packages no longer need it.

The committed Video Player release is enabled independently of any uncommitted
hardware-MJPEG experiment in the working tree.
