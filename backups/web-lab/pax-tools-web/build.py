#!/usr/bin/env python3
"""Build the active PAX launcher sources into WebAssembly."""

import argparse
import concurrent.futures
import hashlib
import json
import os
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser()
p.add_argument(
    "--emcc",
    default=os.environ.get(
        "EMCC",
        shutil.which("emcc")
        or str(Path.home() / "code/emsdk/upstream/emscripten/emcc"),
    ),
)
p.add_argument("--out", type=Path, default=ROOT / "pax-dev/web")
p.add_argument("--jobs", type=int, default=8)
p.add_argument("--app", choices=["launcher", "netsurf"], default="launcher")
a = p.parse_args()
app = ROOT / "pax-modules/apps/launcher_ui"
lv = ROOT / "pax-modules/apps/_shared/lvgl"
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=True)
objdir = ROOT / "pax-tools/.cache/web" / a.app
objdir.mkdir(parents=True, exist_ok=True)
sdk = objdir / "sdk"
sdk.mkdir(exist_ok=True)
shutil.copy2(
    ROOT / "pax-esphome/components/pax_loader/psram_app.h", sdk / "psram_app.h"
)
shutil.copy2(
    ROOT / "pax-esphome/components/pax_loader/pax_theme_profile.h",
    sdk / "pax_theme_profile.h",
)
shutil.copy2(
    ROOT / "pax-esphome/components/pax_loader/pax_wifi_settings.h",
    sdk / "pax_wifi_settings.h",
)
sources = [app / "main.c", ROOT / "pax-tools/web/runtime.c"] + [
    s
    for s in sorted((lv / "src").rglob("*.c"))
    if "drivers" not in s.relative_to(lv / "src").parts
]
flags = [
    "-O2",
    "-DPAX_APP_SIDE=1",
    "-DLV_CONF_INCLUDE_SIMPLE=1",
    "-sUSE_SDL=2",
    f"-I{sdk}",
    f"-I{app}",
    f"-I{lv}",
    f"-I{lv}/src",
]
if a.app == "netsurf":
    import sys

    sys.path.insert(0, str(ROOT / "pax-modules/tools"))
    import build_pax as builder

    manifest = json.loads((ROOT / "pax-modules/apps/netsurf/pax.json").read_text())
    src = (
        ROOT / "pax-modules/.cache" / ("src-netsurf-" + manifest["source"]["ref"][:12])
    )
    if not (src / ".pax-gen/pax_fonts.c").exists():
        raise SystemExit(
            "Prepare the pinned NetSurf sources with build_pax.py netsurf first."
        )
    units, _ = builder.custom_units(manifest, src, objdir)
    compat = builder.get_upstream_compat(manifest)
    # Use musl headers for POSIX APIs, preserving the port's iconv shim.
    include = objdir / "include"
    include.mkdir(exist_ok=True)
    shutil.copy2(ROOT / "pax-modules/apps/netsurf/include/iconv.h", include / "iconv.h")
    prepared = []
    for unit in units:
        if unit.src.name == "pax_syscalls.c":
            continue
        flags = [
            f
            for f in unit.flags
            if not f.startswith(
                (
                    "-march=",
                    "-mabi=",
                    "-mcmodel=",
                    "-I" + str(ROOT / "pax-modules/apps/netsurf/include"),
                )
            )
        ]
        flags += [
            f"-I{ROOT}/pax-modules/apps/netsurf",
            f"-I{include}",
            f"-I{compat}",
            "-include",
            str(compat / "upstream_compat.h"),
        ]
        source = unit.src
        if source.name == "pax_main.c":
            import re

            content = (
                source.read_text()
                .replace(
                    "extern int main(int argc, char **argv);",
                    "extern int netsurf_frontend_main(int argc, char **argv);",
                )
                .replace(
                    "const int rc = main(argc, argv);",
                    "const int rc = netsurf_frontend_main(argc, argv);",
                )
            )
            content = re.sub(
                r"    for \(init_fn \*fn = __init_array_start; fn < __init_array_end; \+\+fn\) \{.*?\n    \}",
                "",
                content,
                flags=re.DOTALL,
            )
            source = objdir / "netsurf_main.c"
            source.write_text(content)
        if source.name == "pax_http.c":
            import re

            content = source.read_text()
            # Fetch uses hostnames, so the browser bridge supplies opaque address tokens.
            content = re.sub(
                r"static bool parse_ipv4\(const char \*s, uint32_t \*ip\)\n\{.*?\n\}",
                "static bool parse_ipv4(const char *s, uint32_t *ip)\n{ return pax_svc->net_resolve(s, ip) == 1; }",
                content,
                flags=re.DOTALL,
            )
            source = objdir / "netsurf_http.c"
            source.write_text(content)
        if unit.src == src / "frontends/framebuffer/gui.c":
            flags += ["-Dmain=netsurf_frontend_main"]
        prepared.append((source, flags))
    base = [
        "-O2",
        "-DPAX_APP_SIDE=1",
        f"-I{sdk}",
        f"-I{ROOT}/pax-modules",
        f"-I{ROOT}/pax-modules/apps/netsurf",
    ]
    prepared += [
        (ROOT / "pax-tools/web/runtime.c", base + ["-sUSE_SDL=2"]),
        (ROOT / "pax-tools/web/netsurf_syscalls.c", base),
    ]
else:
    prepared = [(source, flags) for source in sources]


def compile(unit):
    s, compile_flags = unit
    obj = objdir / (hashlib.sha256(str(s).encode()).hexdigest()[:16] + ".o")
    command = [a.emcc, *compile_flags]
    stamp = obj.with_suffix(".command.json")
    deps = obj.with_suffix(".d")
    if (
        obj.exists()
        and stamp.exists()
        and deps.exists()
        and json.loads(stamp.read_text()) == command
    ):
        names = deps.read_text().replace("\\\n", " ").split(":", 1)[1].split()
        if all(
            Path(n).exists() and Path(n).stat().st_mtime < obj.stat().st_mtime
            for n in names
        ):
            return str(obj)
    subprocess.run(
        [
            a.emcc,
            *compile_flags,
            "-MMD",
            "-MF",
            str(deps),
            "-c",
            str(s),
            "-o",
            str(obj),
        ],
        check=True,
        capture_output=True,
    )
    stamp.write_text(json.dumps(command))
    return str(obj)


try:
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
        objects = list(pool.map(compile, prepared))
    subprocess.run(
        [
            a.emcc,
            *objects,
            *(["-Wl,--wrap=exit"] if a.app == "netsurf" else []),
            "-O2",
            "-sUSE_SDL=2",
            "-sASYNCIFY=1",
            "-sASYNCIFY_STACK_SIZE=65536",
            "-sALLOW_MEMORY_GROWTH=1",
            "-sINITIAL_MEMORY=67108864",
            "-sSTACK_SIZE=1048576",
            "-sMODULARIZE=1",
            "-sEXPORT_NAME=createPax",
            '-sEXPORTED_FUNCTIONS=["_main","_pax_stop"]',
            '-sEXPORTED_RUNTIME_METHODS=["FS","callMain","ccall"]',
            "-sINVOKE_RUN=0",
            "-lidbfs.js",
            "-o",
            str(out / (a.app + ".js")),
        ],
        check=True,
    )
except subprocess.CalledProcessError as e:
    print((e.stderr or b"").decode())
    raise SystemExit(1)
for name in ("index.html", "page.js", "transport.js", "style.css", "test-page.html"):
    shutil.copy2(Path(__file__).parent / name, out / name)
(out / (a.app + "-build.json")).write_text(
    json.dumps(
        {
            "app": a.app,
            "target": "wasm32",
            "source": "pax-modules/apps/"
            + ("launcher_ui" if a.app == "launcher" else "netsurf"),
            "javascript": False,
            "compiler": subprocess.check_output(
                [a.emcc, "--version"], text=True
            ).splitlines()[0],
            "abi_header_sha256": hashlib.sha256(
                (sdk / "psram_app.h").read_bytes()
            ).hexdigest(),
        },
        indent=2,
    )
    + "\n"
)
print(out / "index.html")
