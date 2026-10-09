# Launcher themes

Settings → THEME selects PAX, Emerald, Amber, Violet or Cyberpunk. Use Left/Right, A or touch to cycle. The selected palette is saved on SD and applies to the launcher, slide-down menu and speaker/volume overlay. Firmware reads changes within two seconds.

For a custom scheme, copy `apps/launcher_ui/themes/custom-example.json`, rename it, edit its name and all nine #RRGGBB colors, and put it in `/sd/roms/pax/themes/` (the card’s `roms/pax/themes/` directory). Restart the launcher to discover added files. Names must be printable ASCII, up to 31 characters. Invalid profiles are ignored. Up to 20 profiles are available including the presets.

The saved active palette is `/sd/roms/pax/.launcher_ui_theme.json`. Keep custom files in the themes directory so they remain selectable. Cyberpunk uses a dark violet background, magenta accent and cyan status highlights. Examples are also hosted under the development server’s `themes/` folder.
