"""The compiled launcher discovers a live server and caches its dynamic port."""
from pathlib import Path
import json,os,subprocess,tempfile,threading,unittest
from functools import partial
from http.server import ThreadingHTTPServer
from test_supply_server import server as supply
ROOT=Path(__file__).resolve().parents[1]
BINARY=os.environ.get('PAX_LAUNCHER_BINARY')
@unittest.skipUnless(BINARY,'set PAX_LAUNCHER_BINARY for UDP/HTTP integration')
class SupplyEmulated(unittest.TestCase):
 def test_discovery_uses_current_http_port_and_lists_apps(self):
  with tempfile.TemporaryDirectory() as td:
   folder=Path(td);packages=folder/'packages';packages.mkdir();(packages/'found.pax').write_bytes(b'PAX')
   sd=folder/'sd';apps=sd/'roms/pax';apps.mkdir(parents=True)
   http=ThreadingHTTPServer(('127.0.0.1',0),partial(supply.PaxRequestHandler,directory=str(packages)))
   thread=threading.Thread(target=http.serve_forever,daemon=True);thread.start()
   discovery=supply.SupplyDiscovery(http.server_port,'127.0.0.1',0)
   try:
    script=folder/'input.json';script.write_text(json.dumps([dict(at=2,type='touch',x=90,y=30,down=True),dict(at=2.6,type='touch',x=90,y=30,down=False)]))
    report=folder/'report.json'
    result=subprocess.run([str(ROOT.parent/'pax-tools/python-desktop/bin/python'),str(ROOT/'tests/launcher_supply_fixture.py'),'emulated','--pax',str(Path(BINARY).resolve()),'--headless','--seconds','10','--sd-root',str(sd),'--input-script',str(script),'--report',str(report)],env={**os.environ,'SDL_AUDIODRIVER':'dummy','PAX_TEST_DISCOVERY_PORT':str(discovery.socket.getsockname()[1])},capture_output=True,text=True,timeout=40)
    self.assertEqual(result.returncode,0,result.stderr[-2000:]);data=json.loads(report.read_text());self.assertIsNone(data['error'])
    self.assertEqual((apps/'.launcher_ui_network_url.txt').read_text(),f'http://127.0.0.1:{http.server_port}/')
    self.assertRegex(result.stderr,r'launcher_ui LAN catalog: bytes=\d+ apps=1')
    self.assertGreater(data['services'].get('net_udp_recv',0),0);self.assertEqual(data['services'].get('net_udp_open'),1);self.assertGreaterEqual(data['services'].get('net_udp_close',0),1)
   finally:discovery.close();http.shutdown();http.server_close();thread.join()
if __name__=='__main__':unittest.main()
