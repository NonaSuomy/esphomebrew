"""Optional full RV32 smoke test: PAX_LAUNCHER_BINARY=/.../launcher_ui.pax.

Run with pax-tools/python-desktop/bin/python and the desktop host installed.
The server deliberately never replies before navigation and app shutdown.
"""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import json
import os
import subprocess
import tempfile
import threading
import unittest

ROOT=Path(__file__).resolve().parents[1]
BINARY=os.environ.get('PAX_LAUNCHER_BINARY')

@unittest.skipUnless(BINARY, 'set PAX_LAUNCHER_BINARY to exercise the compiled launcher')
class EmulatedIOTests(unittest.TestCase):
    def test_stalled_catalog_can_be_cancelled_and_worker_is_deleted(self):
        requested=threading.Event()
        release=threading.Event()
        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                requested.set()
                release.wait(30)
                try:
                    self.send_response(200);self.end_headers()
                    self.wfile.write(b'<a href="stale.pax">stale.pax</a>')
                except (BrokenPipeError,ConnectionResetError):
                    pass
            def log_message(self,*args):pass
        server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
        server.daemon_threads=True
        thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
        try:
            with tempfile.TemporaryDirectory() as td:
                folder=Path(td);sd=folder/'sd';apps=sd/'roms/pax';apps.mkdir(parents=True)
                for i in range(30):
                    (apps/f'app{i:02d}.pax').write_bytes(b'PAX')
                    (apps/f'app{i:02d}.json').write_text(json.dumps({'title':f'App {i:02d}'}))
                (apps/'.launcher_ui_network_mode.txt').write_text('manual')
                (apps/'.launcher_ui_network_url.txt').write_text(f'http://127.0.0.1:{server.server_port}/')
                events=[]
                for t,x in [(1,680),(2,90),(3,40),(4,290)]:
                    events.extend([dict(at=t,type='touch',x=x,y=30,down=True),dict(at=t+.15,type='touch',x=x,y=30,down=False)])
                script=folder/'input.json';script.write_text(json.dumps(events))
                report=folder/'report.json'
                host=ROOT.parent/'pax-tools/desktop/host.py'
                python=ROOT.parent/'pax-tools/python-desktop/bin/python'
                result=subprocess.run([str(python),str(host),'emulated','--pax',str(Path(BINARY).resolve()),'--headless','--seconds','6','--sd-root',str(sd),'--input-script',str(script),'--report',str(report)],env={**os.environ,'SDL_AUDIODRIVER':'dummy'},capture_output=True,text=True,timeout=20)
                self.assertEqual(result.returncode,0,result.stderr[-3000:])
                data=json.loads(report.read_text())
                self.assertTrue(requested.is_set(),result.stderr)
                self.assertIsNone(data['error'])
                self.assertEqual(data['services'].get('net_udp_open',0),0)  # Explicit manual URL skips discovery.
                self.assertEqual(data['services'].get('task_create'),1)
                self.assertEqual(data['services'].get('task_delete'),1)
                self.assertGreater(data['services'].get('input_mouse_read',0),20)
                self.assertRegex(result.stderr,r'launcher_ui I/O: \d+ jobs, [1-9]\d* cancelled')
                self.assertNotIn("name='stale.pax'",result.stderr)
                self.assertLess(data['elapsed_seconds'],12) # shorter than HTTP idle timeout
        finally:
            release.set();server.shutdown();server.server_close();thread.join()

if __name__=='__main__':unittest.main()
