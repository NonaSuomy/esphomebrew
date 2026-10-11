#!/usr/bin/env python3
"""Write the store catalog (index.html + store.json) from dist/build.json.

The ESPHome pax_loader reads `catalog_url` as HTML: every href ending in
.pax becomes a button labelled with the link's file name, and relative links
resolve against the catalog URL. Each app's .pax is copied into the site
itself (e.g. lvgl-0.3.0.pax) and linked relatively, so devices download
from GitHub Pages. Pages uses Let's Encrypt (ISRG Root X1), which the ESP-IDF
certificate bundle verifies; github.com release downloads chain to Sectigo's
newer ECC root and fail verification on-device (seen on ESP-IDF 6.1). The
GitHub Release stays the versioned archive and is linked by its tag page, which
is not a .pax link. The loader downloads at most 64 KB of catalog, so the page
stays small and holds no other .pax links.

Apps that need files on the card (a game's data) list them in pax.json under
"data", pinned by commit, size and sha256. Those files are downloaded, checked
and published under data/<app>/, and each such app gets a plain-text file list
next to its .pax (doom-0.2.1.pax -> doom-0.2.1.files). The
loader reads that list before launching and fetches whatever the card is
missing. One line per file:

    <size> <sha256> <target path under the data root> <https URL>

    python3 tools/make_catalog.py --repo OWNER/REPO --dist dist --out site [--data-cache DIR]
"""

from __future__ import annotations

import argparse
from datetime import datetime
import base64
import hashlib
import html
import json
import shutil
import sys
import urllib.parse
import urllib.request
from pathlib import Path

MAX_CATALOG_BYTES = 64 * 1024
DATA_LIST_HEADER = "# pax-data 1"


def release_tag(app: dict) -> str:
    return f"{app['name']}-v{app['version']}"


def asset_name(app: dict) -> str:
    return f"{app['name']}-{app['version']}.pax"


def asset_url(repo: str, app: dict) -> str:
    """Archived copy on the GitHub Release."""
    # Previously released versions retain their original asset extension. Pages
    # publishes the same ABI-1 bytes as .pax; do not invent a missing release asset.
    suffix = ".papp" if Path(app.get("file", "")).suffix == ".papp" else ".pax"
    archived = f"{app['name']}-{app['version']}{suffix}"
    return f"https://github.com/{repo}/releases/download/{release_tag(app)}/{archived}"


def pages_base(repo: str) -> str:
    owner, name = repo.split("/", 1)
    return f"https://{owner.lower()}.github.io/{name}/"


def pages_url(repo: str, app: dict) -> str:
    """Where devices download it: the copy on GitHub Pages next to index.html."""
    return pages_base(repo) + asset_name(app)


def data_list_name(app: dict) -> str:
    """The app's data file list; the loader finds it by swapping .pax for .files."""
    return f"{app['name']}-{app['version']}.files"


def data_site_path(app: dict, target: str) -> str:
    return f"data/{app['name']}/{target}"


def raw_url(data: dict, path: str) -> str:
    """The pinned file in its source repository."""
    repo = data["repo"].removeprefix("https://github.com/").rstrip("/")
    return f"https://raw.githubusercontent.com/{repo}/{data['ref']}/{urllib.parse.quote(path)}"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def fetch_verified(url: str, size: int, sha256: str, dest: Path, opener=urllib.request.urlopen) -> None:
    """Download url to dest, refusing anything but the pinned size and sha256."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    part = dest.with_name(dest.name + ".part")
    digest, got = hashlib.sha256(), 0
    with opener(url) as response, part.open("wb") as out:
        while chunk := response.read(1 << 20):
            got += len(chunk)
            if got > size:
                break
            digest.update(chunk)
            out.write(chunk)
    if got != size or digest.hexdigest() != sha256:
        part.unlink(missing_ok=True)
        raise ValueError(f"{url}: expected {size} bytes sha256 {sha256}, got {got} bytes sha256 {digest.hexdigest()}")
    part.replace(dest)


def publish_data(repo: str, app: dict, out: Path, cache: Path | None, opener=urllib.request.urlopen) -> str:
    """Put the app's pinned data files into the site and return its file list."""
    data = app["data"]
    lines = [DATA_LIST_HEADER, f"# {app['name']} {app['version']}: {data['license']}"]
    for f in data["files"]:
        dest = out / data_site_path(app, f["target"])
        cached = cache / f["sha256"] if cache else None
        if cached and cached.is_file() and cached.stat().st_size == f["size"] and sha256_file(cached) == f["sha256"]:
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(cached, dest)
        else:
            fetch_verified(raw_url(data, f["path"]), f["size"], f["sha256"], dest, opener)
            if cached:
                cached.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(dest, cached)
        url = pages_base(repo) + urllib.parse.quote(data_site_path(app, f["target"]))
        lines.append(f"{f['size']} {f['sha256']} {f['target']} {url}")
    return "\n".join(lines) + "\n"


def data_size(app: dict) -> int:
    return sum(f["size"] for f in app.get("data", {}).get("files", []))


def release_page(repo: str, app: dict) -> str:
    return f"https://github.com/{repo}/releases/tag/{release_tag(app)}"


def sidecar_name(app: dict) -> str:
    """The app's listing as JSON; the loader finds it by swapping .pax for .json."""
    return f"{app['name']}-{app['version']}.json"


def icon_name(app: dict) -> str:
    return f"{app['name']}-{app['version']}.png"


def screenshot_name(app: dict, index: int) -> str:
    return f"{app['name']}-{app['version']}-screen{index + 1}.png"


LISTING_FIELDS = ("author", "category", "license", "about", "changelog", "controls", "upstream", "canvas",
                  "canvas_recommended", "extract")


def requires_list(app: dict) -> list[dict]:
    """What the app needs on the card, for the store's tick-or-cross check: its
    "requires" from pax.json, then each data file the store downloads for it
    (as /sd/<target>, marked "download")."""
    required = list(app.get("requires", []))
    for f in app.get("data", {}).get("files", []):
        path = "/sd/" + f["target"]
        if not any(entry["path"] == path for entry in required):
            required.append({"path": path, "download": True})
    return required


def store_entry(repo: str, a: dict) -> dict:
    """One app in store.json and in its sidecar: what a store UI shows before downloading."""
    return {
        "name": a["name"],
        "title": a["title"],
        "version": a["version"],
        "description": a.get("description", ""),
        **{key: a[key] for key in LISTING_FIELDS if key in a},
        **({"requires": requires_list(a)} if requires_list(a) else {}),
        **({"icon": {**a["icon"], "url": pages_base(repo) + icon_name(a)}} if a.get("icon") else {}),
        # Screenshots are big, so only their URLs; the icon stays inline.
        **({"screenshots": [{"width": s["width"], "height": s["height"], "url": pages_base(repo) + screenshot_name(a, i)}
                            for i, s in enumerate(a["screenshots"])]} if a.get("screenshots") else {}),
        "file": asset_name(a),
        "url": pages_url(repo, a),
        "info_url": pages_base(repo) + sidecar_name(a),
        "release_url": asset_url(repo, a),
        "size": a["size"],
        **({"built_at": a["built_at"]} if a.get("built_at") else {}),
        "data_size": data_size(a),
        "sha256": a["sha256"],
        "abi": a["abi"],
        "source": a["source"],
        **({"data": {
            "list_url": pages_base(repo) + data_list_name(a),
            "license": a["data"]["license"],
            "source": {"repo": a["data"]["repo"], "ref": a["data"]["ref"]},
            "files": [{"target": f["target"], "size": f["size"], "sha256": f["sha256"],
                       "url": pages_base(repo) + urllib.parse.quote(data_site_path(a, f["target"]))}
                      for f in a["data"]["files"]],
        }} if a.get("data") else {}),
    }


def render(repo: str, apps: list[dict]) -> str:
    # Version the script so an older browser cache cannot keep old install behavior.
    script = Path(__file__).resolve().parent.parent / "web-store" / "store-install.js"
    script_version = hashlib.sha256(script.read_bytes()).hexdigest()[:12]
    rows = []
    for app in sorted(apps, key=lambda a: a["name"]):
        # Keep icons external and exactly one PAX link per app for the loader.
        icon = (f'<img src="{html.escape(icon_name(app))}" width="48" height="48" alt="" loading="lazy">'
                if app.get("icon") else '<span class="app-icon" aria-hidden="true">PAX</span>')
        description = app.get("description", "").replace("PAPP", "PAX").replace("papp", "pax")
        rows.append(
            '<li class="app-card"><div class="app-heading">' + icon
            + f'<div><h3>{html.escape(app["title"])}</h3><span class="version">Version {html.escape(app["version"])}</span></div></div>'
            + f'<p class="description">{html.escape(description)}</p>'
            + f'<p class="meta">{app["size"] // 1024} KB'
            + (f' &middot; {data_size(app) / 1e6:.1f} MB included data' if app.get("data") else "")
            + '</p><div class="app-actions">'
            + f'<a class="download" href="{html.escape(asset_name(app))}" data-pax-title-url="{urllib.parse.quote(app["title"], safe="")}" data-pax-size="{app["size"]}" data-pax-mtime="{app.get("built_at", 0)}">Download PAX<span class="sr-only"> for {html.escape(app["title"])}</span></a>'
            + f'<button class="install-app" type="button" data-app="{html.escape(app["name"])}">Install on device</button>'
            + f'<a class="release" href="{html.escape(release_page(repo, app))}">Release notes</a></div>'
            + f'<p class="file">{html.escape(asset_name(app))}<br>SHA256 <code>{app["sha256"][:12]}</code></p></li>'
        )
    styles = """
:root{color-scheme:dark}[hidden]{display:none!important}*{box-sizing:border-box}body{margin:0;background:#061326;color:#d7f3ff;font:16px/1.6 system-ui,sans-serif}a{color:#55d6ff;text-underline-offset:4px}a:hover{color:#b9f1ff}a:focus-visible{outline:3px solid #b9f1ff;outline-offset:4px}header{border-bottom:1px solid #183954}.header-inner,main,footer{max-width:1180px;margin:auto;padding:24px 32px}.header-inner{display:flex;align-items:center;justify-content:space-between;gap:20px}.brand{font-size:12px;font-weight:800;letter-spacing:.14em;text-decoration:none}nav{display:flex;gap:24px;font-size:14px}nav a{text-decoration:none}.hero{padding:40px 0 44px}.eyebrow{color:#55d6ff;font-size:13px;font-weight:700;letter-spacing:.13em;text-transform:uppercase;margin:0 0 16px}h1{font-size:clamp(48px,7vw,84px);line-height:1.06;letter-spacing:-.055em;margin:0 0 24px}.intro{font-size:20px;color:#a4c6db;max-width:700px;margin:0 0 28px}.primary,.download,.install-app{display:inline-block;background:#55d6ff;color:#061326;font-weight:700;text-decoration:none;border-radius:9px}.primary{padding:14px 22px}.primary:hover,.download:hover,.install-app:hover{background:#b9f1ff;color:#061326}.section-heading{display:flex;align-items:center;justify-content:space-between;gap:16px;border-top:1px solid #183954;padding-top:28px;margin-bottom:20px}h2{font-size:24px;margin:0}.count{font-size:13px;color:#9bb9cc;border:1px solid #204966;border-radius:30px;padding:4px 12px}.apps{list-style:none;padding:0;margin:0;display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:20px}.app-card{background:#0b2138;border:1px solid #204966;border-radius:16px;padding:20px;display:flex;flex-direction:column;min-width:0}.app-card:hover{border-color:#55d6ff}.app-heading{display:flex;gap:14px;align-items:center}.app-heading img,.app-icon{flex-shrink:0;border-radius:10px;image-rendering:pixelated}.app-icon{width:48px;height:48px;display:grid;place-items:center;background:#10314e;color:#55d6ff;font-size:11px;font-weight:800;letter-spacing:.08em}h3{font-size:20px;line-height:1.3;margin:0 0 3px}.version{color:#849eae;font-size:13px}.description{color:#a4c6db;font-size:14px;overflow-wrap:anywhere;margin:20px 0}.meta{color:#9bb9cc;font-size:13px;margin:auto 0 14px}.app-actions{display:flex;align-items:center;gap:16px;flex-wrap:wrap}.download,.install-app{padding:9px 14px;font-size:14px}.install-app{border:0;font-family:inherit;cursor:pointer}.download{background:#10314e;color:#d7f3ff}.connection{background:#081c30;border:1px solid #204966;border-radius:16px;padding:24px;margin-bottom:32px}.connection p{color:#a4c6db}.connection label{display:block;font-size:14px;color:#a4c6db}.connection input[type=text]{background:#10314e;border:1px solid #204966;border-radius:8px;color:#d7f3ff;padding:12px;font:inherit;width:100%;max-width:400px;margin:8px 0 16px}.connection button{border:0;font:inherit;cursor:pointer;margin:0 12px 12px 0}.connection .ack{display:flex;align-items:center;gap:10px}.connection progress{width:100%;height:12px;margin-top:16px}button:disabled{opacity:.5;cursor:wait}#install-status{overflow-wrap:anywhere;min-height:1.6em}.release{font-size:13px}.file{color:#849eae;font-size:12px;overflow-wrap:anywhere;margin:16px 0 0}code{font-size:12px;background:#10314e;border-radius:4px;padding:3px 6px}footer{color:#849eae;font-size:13px;margin-top:32px;border-top:1px solid #183954;display:flex;justify-content:space-between;gap:20px}.sr-only{position:absolute;width:1px;height:1px;padding:0;margin:-1px;overflow:hidden;clip:rect(0,0,0,0);white-space:nowrap;border:0}@media(max-width:1000px){.apps{grid-template-columns:repeat(2,minmax(0,1fr))}}@media(max-width:600px){.header-inner,main,footer{padding:20px}.header-inner{align-items:flex-start;flex-direction:column;gap:12px}nav{gap:20px}.hero{padding:28px 0 36px}.intro{font-size:18px}.apps{grid-template-columns:1fr}footer{flex-direction:column;gap:8px}}
"""
    return (
        '<!doctype html>\n<html lang="en"><head><meta charset="utf-8"><title>PAX Store</title>'
        '<meta name="viewport" content="width=device-width,initial-scale=1">'
        '<meta name="theme-color" content="#061326">'
        '<meta name="description" content="Games, emulators and apps for your PAX device. Download apps or install firmware for your ESP32-P4 board.">'
        f'<style>{styles}</style><script defer src="store-install.js?v={script_version}"></script></head>\n<body>'
        '<header><div class="header-inner"><a class="brand" href="./">PAX / ESPHOMEBREW</a>'
        '<nav aria-label="Main navigation"><a href="install/">Install firmware</a>'
        f'<a href="https://github.com/{html.escape(repo)}">GitHub</a></nav></div></header>'
        '<main><section class="hero" aria-labelledby="store-title"><p class="eyebrow">ESP32-P4 apps</p>'
        '<h1 id="store-title">PAX Store</h1><p class="intro">Games, emulators and creative tools for your PAX device. Browse the apps below or get your board ready with the firmware installer.</p>'
        '<a class="primary" href="install/">Install PAX firmware</a></section>'
        '<section class="connection" id="store-connection" aria-labelledby="connection-title"><h2 id="connection-title">Install on your device</h2>'
        '<p>Connect by USB serial, or enter your device address for Wi-Fi. Then choose Install on device beside an app. Keep the SD card inserted and close any running app.</p>'
        '<button class="primary" id="serial-connect" type="button">Connect USB serial</button>'
        '<button class="primary" id="serial-disconnect" type="button" hidden>Disconnect serial</button>'
        '<button class="primary" id="close-app" type="button">Close running app via serial</button>'
        '<label for="device-address">Wi-Fi device IP address or .local name</label>'
        '<input type="text" id="device-address" placeholder="192.168.0.241" autocomplete="off" spellcheck="false">'
        '<label class="ack"><input type="checkbox" id="install-replace">Replace matching SD files during serial installation</label>'
        '<p>USB serial transfers work without Wi-Fi in desktop Chrome or Edge and require current PAX firmware. Wi-Fi installation continues on your device page. Included game data is installed with the app; other required ROMs and game files are listed in each app description.</p>'
        '<div aria-live="polite"><progress id="install-progress" max="100" value="0"></progress><p id="install-status">Choose a connection to get started.</p></div></section>'
        f'<section aria-labelledby="apps-title"><div class="section-heading"><h2 id="apps-title">Explore apps</h2><span class="count">{len(apps)} apps</span></div>'
        '<ul class="apps">\n' + "\n".join(rows) + '\n</ul></section></main>'
        '<footer><span>PAX apps for ESP32-P4</span><a href="store.json">App catalog JSON</a></footer></body></html>\n'
    )


def build_timestamp(value: str) -> int:
    """An actual CI build start, never the artifact extraction/publish time."""
    try:
        stamp = datetime.fromisoformat(value.replace("Z", "+00:00"))
        if stamp.tzinfo is None:
            raise ValueError("timezone required")
        seconds = int(stamp.timestamp())
        if seconds <= 0 or seconds > 253402300799:
            raise ValueError("timestamp out of range")
        return seconds
    except (ValueError, OverflowError) as error:
        raise argparse.ArgumentTypeError("build time must be an ISO-8601 timestamp with timezone") from error


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", required=True, help="OWNER/REPO that hosts the releases")
    parser.add_argument("--dist", type=Path, default=Path("dist"))
    parser.add_argument("--out", type=Path, default=Path("site"))
    parser.add_argument("--data-cache", type=Path, help="keep downloaded data files here (by sha256) between runs")
    parser.add_argument("--built-at", type=build_timestamp, help="CI build start timestamp (ISO-8601 with timezone); Store list date")
    args = parser.parse_args()

    # Apps marked "publish": false are work in progress: built, but not in the store.
    apps = [a for a in json.loads((args.dist / "build.json").read_text())["apps"] if a.get("publish", True)]
    if args.built_at:
        apps = [{**a, "built_at": args.built_at} for a in apps]
    page = render(args.repo, apps)
    if len(page.encode()) > MAX_CATALOG_BYTES:
        print(f"catalog is {len(page.encode())} bytes; the device reads at most {MAX_CATALOG_BYTES}", file=sys.stderr)
        return 1

    store = {"apps": [store_entry(args.repo, a) for a in sorted(apps, key=lambda a: a["name"])]}
    args.out.mkdir(parents=True, exist_ok=True)
    for a, entry in zip(sorted(apps, key=lambda a: a["name"]), store["apps"]):
        # Build manifests may still refer to .papp artifacts during migration.
        # ABI-1 bytes and checksums are unchanged; only the public filename changes.
        shutil.copyfile(args.dist / a["file"], args.out / asset_name(a))
        # The app's full listing next to its .pax (swap .pax for .json), so a
        # device reading the HTML catalog, or a folder of .pax files, can show it.
        (args.out / sidecar_name(a)).write_text(json.dumps(entry, indent=2) + "\n")
        if a.get("icon"):
            (args.out / icon_name(a)).write_bytes(base64.b64decode(a["icon"]["base64"]))
        for i, shot in enumerate(a.get("screenshots", [])):
            (args.out / screenshot_name(a, i)).write_bytes(base64.b64decode(shot["base64"]))
        if a.get("data"):
            (args.out / data_list_name(a)).write_text(publish_data(args.repo, a, args.out, args.data_cache))
            print(f"data: {a['name']}: {len(a['data']['files'])} file(s), {data_size(a):,} bytes")
    (args.out / "index.html").write_text(page)
    (args.out / "store.json").write_text(json.dumps(store, indent=2) + "\n")
    print(f"catalog: {len(apps)} app(s), {len(page.encode())} bytes -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
