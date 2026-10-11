#!/usr/bin/env python3
"""Serve PAX packages with live size/date metadata in directory links."""
import argparse
from datetime import datetime, timezone
import html
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import io
import os
from pathlib import Path
import urllib.parse

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
    args=parser.parse_args()
    if not args.directory.is_dir():parser.error('Directory does not exist')
    from functools import partial
    server=ThreadingHTTPServer((args.bind,args.port),partial(PaxRequestHandler,directory=str(args.directory.resolve())))
    print(f'Serving PAX file details on {args.bind}:{server.server_port}',flush=True)
    try:server.serve_forever()
    except KeyboardInterrupt:pass
    finally:server.server_close()

if __name__=='__main__':main()
