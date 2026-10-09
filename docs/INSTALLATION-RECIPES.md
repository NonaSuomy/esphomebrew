# PAX installation recipes

148 recipes cover all 146 compiled apps and engine packages, plus the preserved War1gus and Wargus source variants. Each recipe records the actual storage paths, required files, optional files, setup instructions and supported download/extraction choices. A recipe is not proof that a game has been tested on hardware.

In the launcher, select an installed app and choose **GET GAME DATA**. In recipe.pax, **Prepare folders / read instructions** creates the folders and opens setup instructions. Download choices fetch the listed files or ZIP and verify their size and SHA-256 when provided. Local ISO/CUE extraction remains available for Red Alert. Existing nonempty files are kept; failed new outputs are removed.

For a mounted card or USB drive on your computer:

```sh
python3 ~/code/pax/pax-recipe/tools/prepare_storage.py list opentyrian
python3 ~/code/pax/pax-recipe/tools/prepare_storage.py prepare opentyrian --root /path/to/card
python3 ~/code/pax/pax-recipe/tools/prepare_storage.py install opentyrian --root /path/to/card --choice 0
python3 ~/code/pax/pax-recipe/tools/prepare_storage.py check opentyrian --root /path/to/card
```

Use `list APP` to see numbered choices and manual steps. ScummVM choice 0 installs shared GUI/engine resources; engines with a freeware package have another choice for game data. Keep commercial game files and ROM/BIOS files on your own media. No generic BIOS or commercial ROM downloads are included. `check` checks file presence, not full game compatibility. Virtual `/sd/` or `/usb0/` paths map to the selected mounted volume root in the computer helper; device recipes currently target SD.

OpenTyrian uses `/sd/tyrian/data`. ScummVM uses `/sd/scummvm` for resources and `/sd/roms/scummvm` for games. Emulator recipes state their ROM formats and any required BIOS. Self-contained apps only need their compiled `.pax`. Stargus needs Startool conversion plus generated media companions. Tiberian Sun and preserved Warcraft variants are explicitly marked as unfinished or processor-dependent.

Canonical JSON sources live with each app under pax-modules/apps. The links here provide one index. build_pax.py embeds manifest recipes, and pax-tools/artifact-index.py refreshes native and engine sidecars after builds. The standalone GitHub snapshot includes the same sources and storage helper.

Canonical registry: [install-recipes.json](../install-recipes.json). Read each registry path relative to pax-modules.
