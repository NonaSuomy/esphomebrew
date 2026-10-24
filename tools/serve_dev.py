#!/usr/bin/env python3
"""Serve PAX packages with live size/date metadata in directory links."""
import argparse
from datetime import datetime, timezone
import html
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import io
import json
import os
import socket
import struct
import threading
from pathlib import Path
import urllib.parse

DISCOVERY_PORT = 18732
DISCOVERY_QUERY = b'PAXFIND1'
DISCOVERY_REPLY = b'PAXHERE1'

class SupplyDiscovery:
    """Reply on the request's interface/address; never guess the host's LAN IP."""
    def __init__(self, http_port, bind='0.0.0.0', port=DISCOVERY_PORT):
        self.http_port = http_port
        self.stop = threading.Event()
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            self.socket.bind((bind, port))
            self.socket.settimeout(0.2)
        except BaseException:
            self.socket.close()
            raise
        self.thread = threading.Thread(target=self.run, name='pax-supply-discovery', daemon=True)
        self.thread.start()

    def run(self):
        while not self.stop.is_set():
            try:
                data, peer = self.socket.recvfrom(256)
                if len(data) == 12 and data[:8] == DISCOVERY_QUERY:
                    self.socket.sendto(DISCOVERY_REPLY + data[8:12] + struct.pack('<H', self.http_port), peer)
            except socket.timeout:
                continue
            except OSError:
                if not self.stop.is_set():
                    self.stop.wait(0.2)

    def close(self):
        self.stop.set()
        self.thread.join(timeout=1)
        self.socket.close()

class PaxRequestHandler(SimpleHTTPRequestHandler):
    def handle(self):
        try:
            super().handle()
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            # A browser/device may stop a download or leave before headers arrive.
            # Keep other server errors visible instead of suppressing all OSErrors.
            self.close_connection = True
            self.log_message('Client disconnected; request cancelled')

    def list_directory(self, path):
        try:
            names=sorted(os.listdir(path),key=str.casefold)
        except OSError:
            self.send_error(404,'Directory unavailable')
            return None
        title=html.escape(urllib.parse.unquote(self.path))
        rows=['<!DOCTYPE html><html><head><meta charset="utf-8"><title>PAX '+title+'</title></head>',
              '<body><h1>PAX files: '+title+'</h1><p>File dates are modified times in UTC.</p><ul>']
        for name in names:
            full=Path(path)/name
            try:stat=full.stat()
            except OSError:continue
            directory=full.is_dir()
            label=name+('/' if directory else '')
            href=urllib.parse.quote(name, safe='')+('/' if directory else '')
            metadata=''
            detail=''
            if full.is_file():
                mtime=max(0,int(stat.st_mtime))
                metadata=f' data-pax-size="{stat.st_size}" data-pax-mtime="{mtime}"'
                if full.suffix == '.pax':
                    try:
                        sidecar = full.with_suffix('.json')
                        if sidecar.stat().st_size <= 204800:
                            info = json.loads(sidecar.read_text(encoding='utf-8'))
                            app_title = info.get('title') if isinstance(info, dict) else None
                            if isinstance(app_title, str) and app_title.strip() and len(app_title.encode('utf-8')) < 64 and not any(ord(c) < 32 or ord(c) == 127 for c in app_title):
                                metadata += f' data-pax-title-url="{urllib.parse.quote(app_title, safe="")}"'
                    except (OSError, UnicodeError, ValueError):
                        pass  # Missing/broken metadata leaves the filename usable.
                try:date=datetime.fromtimestamp(mtime,timezone.utc).strftime('%Y-%m-%d %H:%M UTC') if mtime else 'date unavailable'
                except (ValueError,OverflowError,OSError):date='date unavailable'
                detail=f' — {stat.st_size} bytes | {date}'
            rows.append(f'<li><a href="{html.escape(href,quote=True)}"{metadata}>{html.escape(label)}</a>{detail}</li>')
        rows.append('</ul></body></html>')
        data='\n'.join(rows).encode('utf-8')
        self.send_response(200)
        self.send_header('Content-Type','text/html; charset=utf-8')
        self.send_header('Content-Length',str(len(data)))
        self.send_header('Cache-Control','no-store')
        self.end_headers()
        return io.BytesIO(data)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('port',type=int,nargs='?',default=8000)
    parser.add_argument('--directory',type=Path,required=True)
    parser.add_argument('--bind',default='0.0.0.0')
    parser.add_argument('--no-discovery', action='store_true', help='Disable local PAX Supply discovery')
    args=parser.parse_args()
    if not args.directory.is_dir():parser.error('Directory does not exist')
    from functools import partial
    server=ThreadingHTTPServer((args.bind,args.port),partial(PaxRequestHandler,directory=str(args.directory.resolve())))
    discovery = None
    if not args.no_discovery:
        try:
            discovery = SupplyDiscovery(server.server_port, args.bind)
            print(f'PAX Supply discovery enabled on UDP {DISCOVERY_PORT}', flush=True)
        except OSError as error:
            print(f'PAX Supply discovery unavailable: {error}; manual URL still works', flush=True)
    print(f'Serving PAX file details on {args.bind}:{server.server_port}',flush=True)
    try:server.serve_forever()
    except KeyboardInterrupt:pass
    finally:
        if discovery:discovery.close()
        server.server_close()

if __name__=='__main__':main()
