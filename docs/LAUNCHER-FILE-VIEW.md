# Launcher file display

Settings includes **HIDE .PAX EXTENSION** and **FILE DETAILS IN LIST**. Both are off by default and are saved together on SD in `/sd/roms/pax/.launcher_ui_list_view`.

Hide .pax extension changes only the visible app-list label. Filenames, package paths, launching, uninstalling and download targets keep their original values.

File details in list shows the package size in bytes and its modified date/time in UTC at the right end of the app row. Local SD and USB entries use the existing file-stat service. Network entries served by `pax-tools/serve-dev.sh` show live server file sizes and modified times from directory metadata. Restart older servers with this script and use the updated launcher. Other servers, store and Archive entries can still show “File details unavailable” when metadata is absent; browsing does not download every package.

The app details page now includes the full file path, actual local size and modified date, even when the app has no JSON sidecar. Remote entries with an installed copy also show the installed path. Existing version, description, controls and required-data details remain available. Enable **APP DETAILS ON SELECT** to open this page when selecting an app.

The Network list shows package filenames and file details. Selecting an app reads its matching JSON sidecar for title, version, description, controls and installation information. The entire basename must match: `gb.pax` uses `gb.json`, including when stored in a nested folder.

The file system provides modified time only. Zero, negative or out-of-range timestamps display “Unavailable”; no creation date is invented. Dates are formatted as `YYYY-MM-DD HH:MM UTC`. The system’s reported storage timestamps determine their accuracy.

Controller Up/Down reaches the new settings, A toggles them, and touch switches work too. Empty-list toolbar navigation remains available.

The updated launcher and server passed 236 module tests, including network metadata, preference round trips, filename/path preservation, leap-year dates, 64-bit sizes and settings focus order. Controller and visual layout checks on hardware are tracked in workspace validation records.

If the LAN changes, put the development server URL (including trailing `/`) in the SD file `roms/pax/.launcher_ui_network_url.txt`, for example `http://192.168.0.162:8000/`, then restart the launcher. This replaces the historical Ethernet default without rebuilding.

With list details enabled, app names stay large on the left and file size/date appear at the right end of the same row. Long names are shortened to keep the two columns from overlapping.
