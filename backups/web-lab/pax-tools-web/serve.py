#!/usr/bin/env python3
"""Serve the generated PAX Web Lab on loopback."""

import argparse
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument("--port", type=int, default=8767)
p.add_argument(
    "--directory",
    type=Path,
    default=Path(__file__).resolve().parents[2] / "pax-dev/web",
)
a = p.parse_args()


class Handler(SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def copyfile(self, source, outputfile):
        try:
            super().copyfile(source, outputfile)
        except (BrokenPipeError, ConnectionResetError):
            pass


print(f"PAX Web Lab: http://127.0.0.1:{a.port}/", flush=True)
ThreadingHTTPServer(
    ("127.0.0.1", a.port), partial(Handler, directory=str(a.directory))
).serve_forever()
