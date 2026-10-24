"""Navigate the compiled picker, then select its manual-entry fallback."""
from pathlib import Path
import json
import os
import re
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
BINARY=os.environ.get('PAX_LAUNCHER_BINARY')
@unittest.skipUnless(BINARY,'set PAX_LAUNCHER_BINARY for the real RV32 picker')
class WifiPickerEmulated(unittest.TestCase):
 def test_controller_scan_and_manual_entry_keep_connection_staged(self):
  with tempfile.TemporaryDirectory() as td:
   folder=Path(td);sd=folder/'sd';(sd/'roms/pax').mkdir(parents=True)
   events=[dict(at=1,type='touch',x=290,y=30,down=True),dict(at=1.15,type='touch',x=290,y=30,down=False)]
   script=folder/'input.json';script.write_text(json.dumps(sorted(events,key=lambda e:e['at'])))
   report=folder/'report.json'
   result=subprocess.run([str(ROOT.parent/'pax-tools/python-desktop/bin/python'),str(ROOT/'tests/launcher_wifi_fixture.py'),'--fixture-navigation','emulated','--pax',str(Path(BINARY).resolve()),'--headless','--seconds','45','--sd-root',str(sd),'--input-script',str(script),'--report',str(report),'--screenshot','/tmp/pax-wifi-picker-emulated.ppm'],env={**os.environ,'SDL_AUDIODRIVER':'dummy'},capture_output=True,text=True,timeout=80)
   self.assertEqual(result.returncode,0,result.stderr[-3000:]);data=json.loads(report.read_text());self.assertIsNone(data['error'])
   self.assertIn('Wi-Fi fixture: scan accepted',result.stderr)
   stats=re.search(r'launcher_ui render:.*keyboard=(\d+)',result.stderr);self.assertIsNotNone(stats,result.stderr[-3000:]);self.assertGreaterEqual(int(stats[1]),2)
   self.assertEqual(data['services'].get('task_delete'),1)
   self.assertGreater(data['frames'],10)
if __name__=='__main__':unittest.main()
