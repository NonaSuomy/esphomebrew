# GitHub Pages PAX repair

The live `NonaSuomy/esphomebrew` main branch previously published only `.papp` links, so the PAX launcher found zero Store apps. The focused repair is commit `869bf35` and is preserved in `papp-to-pax-store.patch`.

The publisher now exposes `.pax` downloads, matching JSON sidecars and `# pax-data 1` file lists, and adds friendly title metadata. Legacy build artifacts retain their ABI-1 bytes, hashes and original GitHub Release URLs. New PAX build artifacts use PAX Release URLs. Catalog-only changes trigger publication from the latest successful build, and the deployment validates every catalog download's size, SHA-256 and JSON sidecar.

The isolated remote repair passed 220 Python tests. The current PAX module/store snapshots passed 246 each. This patch deliberately changes only the live catalog publisher, publication workflow and two relevant test files; the live repository's historical build job and artifact names remain compatible until the full PAX source migration is published.

Deployment: https://github.com/NonaSuomy/esphomebrew/actions/runs/38142486967
Website: https://nonasuomy.github.io/esphomebrew/

## Store file details

Follow-up commit `92d56f2` adds `data-pax-size` and `data-pax-mtime` to each download link. The date is the start timestamp of the successful CI build that produced the package, passed by the publication workflow as `--built-at`. The JSON sidecar also records `built_at`. An unknown date stays zero; the publisher never substitutes the artifact extraction or Pages deployment time.

Preserved patch: `store-file-details.patch`. Deployment: https://github.com/NonaSuomy/esphomebrew/actions/runs/38143098831

## Public build naming and Tulip reproducibility

Commit `d5000f3` sorts MicroPython frozen modules and pins source freezing for Tulip 0.1.6. The full remote app build and version guard passed: https://github.com/NonaSuomy/esphomebrew/actions/runs/38259725763. Repeated local full builds are byte-identical, and the active PAX port is staged in pax-dev with its previous binary preserved. Patch: `tulip-reproducibility.patch`.

Commit `0281146` renames the public workflow to Build PAXs, manifests to pax.json, the build command to build_pax.py, build artifacts to paxs and new release downloads to .pax. Existing immutable releases retain their original files; matching .pax aliases are added only after their SHA-256 is checked. Native legacy SDK/compiler identifiers retain ABI compatibility. The renamed tools passed 221 Python tests and the Tulip binary is unchanged by the naming migration. Patch: `public-pax-build-naming.patch`.

Remote verification for the public naming migration: https://github.com/NonaSuomy/esphomebrew/actions/runs/38261148831 completed successfully. Compilation, the Tulip freezing regression, immutable-version guard and paxs artifact upload passed. Python and host checks also passed in run 38261148922.
