# Launcher file display

Settings includes **HIDE .PAX EXTENSION** and **FILE DETAILS IN LIST**. Both are off by default and are saved together on SD in `/sd/roms/pax/.launcher_ui_list_view`.

Hide .pax extension changes only the visible app-list label. Filenames, package paths, launching, uninstalling and download targets keep their original values.

File details in list adds a second line with the local package size in bytes and its modified date/time in UTC. Local SD and USB entries use the existing file-stat service. Network, store and Archive entries show “File details unavailable” until information is available locally; browsing does not fetch every remote package just to obtain its date.

The app details page now includes the full file path, actual local size and modified date, even when the app has no JSON sidecar. Remote entries with an installed copy also show the installed path. Existing version, description, controls and required-data details remain available. Enable **APP DETAILS ON SELECT** to open this page when selecting an app.

The file system provides modified time only. Zero, negative or out-of-range timestamps display “Unavailable”; no creation date is invented. Dates are formatted as `YYYY-MM-DD HH:MM UTC`. The system’s reported storage timestamps determine their accuracy.

Controller Up/Down reaches the new settings, A toggles them, and touch switches work too. Empty-list toolbar navigation remains available.

The updated launcher compiled and passed the 233-test module suite, including preference round trips, filename/path preservation, leap-year dates, 64-bit sizes and settings focus order. Hardware layout and controls still need device testing.
