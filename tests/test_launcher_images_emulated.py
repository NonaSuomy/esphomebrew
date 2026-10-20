"""Full RV32 private image lifetime with a deterministic decoder fixture."""
from pathlib import Path
import os,json,re,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
BINARY=os.environ.get('PAX_LAUNCHER_BINARY')
@unittest.skipUnless(BINARY,'set PAX_LAUNCHER_BINARY to test image jobs')
class ImageJobsEmulated(unittest.TestCase):
 def test_cancelled_cover_keeps_navigation_alive(self):
  with tempfile.TemporaryDirectory() as td:
   folder=Path(td);sd=folder/'sd';apps=sd/'roms/pax';apps.mkdir(parents=True)
   (apps/'probe.pax').write_bytes(b'PAX')
   (apps/'probe.json').write_text(json.dumps(dict(name='probe',title='Image Probe',version='1.0',base64='iVBORw0KGgoAAAANSUhEUg==')))
   events=[]
   for at,x,y in [(2,200,200),(5,290,30)]:events.extend([dict(at=at,type='touch',x=x,y=y,down=True),dict(at=at+.15,type='touch',x=x,y=y,down=False)])
   script=folder/'input.json';script.write_text(json.dumps(events));report=folder/'report.json'
   result=subprocess.run([str(ROOT.parent/'pax-tools/python-desktop/bin/python'),str(ROOT/'tests/launcher_touch_fixture.py'),'emulated','--pax',str(Path(BINARY).resolve()),'--headless','--seconds','8','--sd-root',str(sd),'--input-script',str(script),'--report',str(report)],env={**os.environ,'SDL_AUDIODRIVER':'dummy','PAX_FIXTURE_PNG':'1','PAX_FIXTURE_CANCEL_IMAGE':'1'},capture_output=True,text=True,timeout=35)
   self.assertEqual(result.returncode,0,result.stderr[-3000:])
   data=json.loads(report.read_text());self.assertIsNone(data['error'])
   self.assertGreater(data['services'].get('png_load_rgb565_blended',0),0,result.stderr)
   self.assertRegex(result.stderr,r'launcher_ui I/O: \d+ jobs, [1-9]\d* cancelled')
   self.assertRegex(result.stderr,r'launcher_ui timing image_job: samples=[1-9]')
   self.assertRegex(result.stderr,r'launcher_ui timing action_to_flush: samples=[1-9]')
   self.assertEqual(data['services'].get('task_delete'),1)
   self.assertGreater(data['frames'],5)
if __name__=='__main__':unittest.main()
