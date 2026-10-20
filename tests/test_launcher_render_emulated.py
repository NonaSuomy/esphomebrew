"""Optional real RV32 widget-lifetime regressions (PAX_LAUNCHER_BINARY)."""
from pathlib import Path
import json,os,re,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
BINARY=os.environ.get('PAX_LAUNCHER_BINARY')

@unittest.skipUnless(BINARY,'set PAX_LAUNCHER_BINARY to exercise compiled rendering')
class RenderEmulatedTests(unittest.TestCase):
    def run_page(self,settings):
        with tempfile.TemporaryDirectory() as td:
            folder=Path(td);sd=folder/'sd';apps=sd/'roms/pax';apps.mkdir(parents=True)
            for i in range(30):
                (apps/f'app{i:02}.pax').write_bytes(b'PAX')
                (apps/f'app{i:02}.json').write_text(json.dumps({'title':f'App {i:02}'}))
            clicks=([(2,290,30),(4,680,97),(5.5,680,97),(7,680,97),(8.5,680,97)] if settings else [(2,680,97),(3.5,680,97),(5,680,97),(6.5,680,97)])
            events=[]
            for at,x,y in clicks:
                events.extend([dict(at=at,type='touch',x=x,y=y,down=True),dict(at=at+.15,type='touch',x=x,y=y,down=False)])
            script=folder/'events.json';script.write_text(json.dumps(events));report=folder/'report.json'
            result=subprocess.run([str(ROOT.parent/'pax-tools/python-desktop/bin/python'),str(ROOT/'tests/launcher_touch_fixture.py'),'emulated','--pax',str(Path(BINARY).resolve()),'--headless','--seconds','10' if settings else '8','--sd-root',str(sd),'--input-script',str(script),'--report',str(report)],env={**os.environ,'SDL_AUDIODRIVER':'dummy'},capture_output=True,text=True,timeout=35)
            self.assertEqual(result.returncode,0,result.stderr[-2000:])
            data=json.loads(report.read_text());self.assertIsNone(data['error'])
            stats=re.search(r'launcher_ui render: builds=(\d+) rows=(\d+) labels=(\d+) styles=(\d+) keyboard=(\d+) metadata=(\d+)',result.stderr)
            self.assertIsNotNone(stats,result.stderr[-2000:])
            builds,rows,labels,styles,keyboard,metadata=map(int,stats.groups())
            # Settings gets one tree on entry; the host's quit signal returns
            # to the list before leaving the app, which accounts for tree 3.
            self.assertEqual(builds,3 if settings else 1)
            self.assertEqual(keyboard,4)
            self.assertGreater(metadata,0)
            self.assertLess(rows,100)
            self.assertEqual(data['services'].get('task_delete'),1)

    def test_list_keyboard_toggles_reuse_widgets(self):self.run_page(False)
    def test_settings_keyboard_toggles_reuse_widgets(self):self.run_page(True)
