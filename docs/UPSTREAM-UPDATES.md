# Updating PAX sources

Work in `apps/`. `legacy/` holds references and is never a build input.

## Linked engine repositories

`apps/scummvm/engine`, `apps/stratagus/engine`, `apps/stargus/data-source`, and the shared/native dependency links expose the original Git checkouts. Editing through either path changes the same files. Their upstream URLs, PAX branches and commits are recorded in `../workspace-lock.json`. Fetch the upstream remote, review the incoming changes, then merge the desired upstream revision into the owning PAX branch. Resolve conflicts there and rebuild the affected application. Do not reset the PAX branch to upstream: that would discard the port changes.

## Copied RetroESP32 cores

`upstream-bases.json` records the repository, base revision, original directory and copied file list for each active core. Our adapters and core changes live in `apps/<app>/`.

1. Fetch the original repository: `git -C upstream/retroesp32 fetch upstream`.
2. Preview an update: `python3 tools/preview_upstream.py c64 --ref upstream/main` (use a real fetched branch or commit).
3. Inspect the preview and its conflicts. Apply the clean patch from the module root with `git apply --check <preview>/update.patch`, then `git apply <preview>/update.patch`. Resolve any conflicted files manually using the candidate files. Rebuild and test the app before committing.

The preview combines the recorded upstream base, the current PAX source and the incoming revision with a three-way merge. It never changes active source files. New upstream files and deletions are listed separately for review because the port may intentionally select only part of a core. After completing and verifying an update, change that app's `ref` in `upstream-bases.json` to the exact incoming commit and record any new selected files. Commit the source and base record together. A conflicted preview has no patch, so a partial update cannot be accidentally applied.

## Manifest-based upstream sources

Each `apps/<app>/pax.json` pins its original source URL and revision. Update that pin deliberately, review the local adapters and patches under the same app directory, and rebuild. These sources are downloaded into `.cache/`; cache contents are replaceable and are not the place for fixes.

## Finish every update

Commit fixes in the owning repository, sync the publishing snapshot with `../pax-tools/sync-store.py`, regenerate the inventory, and refresh the workspace lock and root repository links. Local tests do not establish that remote GitHub Actions have run. Keep a binary backup before replacing test builds in `pax-dev`.
