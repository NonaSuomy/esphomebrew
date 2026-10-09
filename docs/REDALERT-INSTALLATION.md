# Red Alert installation recipe

The canonical recipe is `pax-modules/apps/redalert/install-recipe.json`.
`pax-recipe/recipes/redalert.json` links to it. The manifest embeds this recipe
as `extract` in the built app's JSON; rebuilding cannot drop the recipe.

Open Red Alert in the launcher and choose **GET GAME DATA**. In recipe.pax:
- **Download base game (Soviet disc)** downloads REDALERT.MIX and MAIN.MIX directly to /sd/roms/redalert/.
- **Download Allied disc data (optional)** downloads Allied MAIN.MIX to /sd/roms/redalert/allied/.
- Local CD1.iso, CD2.iso, CD1.zip, CD2.zip, or the legacy cncredalertalliedcd1.zip can be selected from /sd/roms/redalert/incoming/ for ZIP/ISO extraction.

Direct URLs come from https://archive.org/download/CCRedAlert/CD1.iso/ and
https://archive.org/download/CCRedAlert/CD2.iso/ (View Contents). Nested files
use percent-encoded slashes exactly as published by Archive. Files are streamed
by firmware without downloading or locally extracting the entire ISO. Existing
nonempty files are kept. New downloads must match the listed byte size; no
cryptographic checksum is published by the contents page. HTTP failures retain
the local-disc fallback. Direct downloading requires the updated firmware that
allows the recipe app to use the existing Archive service; ABI remains 1.

Validation on 2026-10-09: both contents listings were readable after retries.
Initial byte-range probes returned HTTP 502. Later ordinary HTTP requests
(with a cache-busting query) returned 200 and binary MIX headers for all three
member URLs; only the first 32 bytes were read. Full downloads and device
operation remain unverified. The installer, launcher, and three-button firmware
compiled successfully; 228 module tests passed, including mocked transfer
success/failure, wrong-size cleanup, existing-file preservation, and metadata
retention through build/catalog/listing generation. No game data was
added to Git and no firmware was flashed by this change.
