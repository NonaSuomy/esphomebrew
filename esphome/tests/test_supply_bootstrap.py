"""Exercise firmware bootstrap discovery before any PAX UI has been loaded."""
from pathlib import Path
import socket,struct,subprocess,tempfile,threading,time,unittest
ROOT=Path(__file__).resolve().parents[1]
class Bootstrap(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.folder=tempfile.TemporaryDirectory();cls.root=Path(cls.folder.name);cls.binary=cls.root/'bootstrap'
  cpp=cls.root/'test.cpp';cpp.write_text(r'''
#define local static
#include "pax_supply_bootstrap.h"
#include <cassert>
#include <iostream>
#include <cstdlib>
int main(int argc,char **argv){
 assert(!pax_supply::safe_http("http://"));assert(!pax_supply::safe_http("file:///etc/passwd"));
 assert(!pax_supply::safe_http("http://server/\r\ninjected"));
 assert(pax_supply::launcher_url("http://server:8000/")=="http://server:8000/launcher_ui.pax");
 assert(pax_supply::launcher_url("https://server/path")=="https://server/path/launcher_ui.pax");
 unsigned char p[14]={'P','A','X','H','E','R','E','1',1,0,0,0,0x40,0x1f};
 assert(pax_supply::decode(p,14,1,0xc0000201)=="http://192.0.2.1:8000/");
 assert(pax_supply::decode(p,14,2,0xc0000201).empty());assert(pax_supply::decode(p,13,1,0xc0000201).empty());
 if(argc==3){std::cout<<pax_supply::read_setting(argv[2]);return 0;}
 if(argc==5)std::cout<<pax_supply::resolve("https://fallback/launcher_ui.pax",argv[2],argv[3],static_cast<uint16_t>(atoi(argv[4])),0x7f000001);
}
''')
  subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(ROOT/'components/pax_loader'),str(cpp),'-o',str(cls.binary)],check=True)
 @classmethod
 def tearDownClass(cls):cls.folder.cleanup()
 def test_manual_mode_and_bounded_settings(self):
  started=time.monotonic();out=subprocess.check_output([str(self.binary),'resolve','manual','http://192.0.2.5:8080/','1'],text=True)
  self.assertEqual(out,'http://192.0.2.5:8080/launcher_ui.pax');self.assertLess(time.monotonic()-started,.5)
  p=self.root/'mode';p.write_text('manual\n');self.assertEqual(subprocess.check_output([str(self.binary),'read',str(p)],text=True),'manual')
  p.write_text('x'*300);self.assertEqual(subprocess.check_output([str(self.binary),'read',str(p)],text=True),'')
 def test_live_discovery_rejects_wrong_token_then_uses_actual_port(self):
  with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as server:
   server.bind(('127.0.0.1',0));server.settimeout(2);port=server.getsockname()[1];errors=[]
   def reply():
    try:
     request,peer=server.recvfrom(64);self.assertEqual(request[:8],b'PAXFIND1');self.assertEqual(len(request),12)
     token=struct.unpack('<I',request[8:])[0]
     server.sendto(b'PAXHERE1'+struct.pack('<IH',token^1,8123),peer)
     server.sendto(b'PAXHERE1'+request[8:]+struct.pack('<H',8123),peer)
    except BaseException as error:errors.append(error)
   thread=threading.Thread(target=reply);thread.start()
   try:self.assertEqual(subprocess.check_output([str(self.binary),'resolve','auto','http://192.0.2.9:8000/',str(port)],text=True),'http://127.0.0.1:8123/launcher_ui.pax')
   finally:thread.join()
   if errors:raise errors[0]
 def test_no_server_falls_back_and_finishes(self):
  with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as unused:
   unused.bind(('127.0.0.1',0));port=unused.getsockname()[1]
   started=time.monotonic();out=subprocess.check_output([str(self.binary),'resolve','auto','http://192.0.2.8:9000/',str(port)],text=True)
   self.assertEqual(out,'http://192.0.2.8:9000/launcher_ui.pax');self.assertLess(time.monotonic()-started,2)
 def test_three_button_wiring_and_public_url(self):
  yaml=(ROOT/'packages/pax-three-button-ui.yaml').read_text();self.assertIn('request_launch("pax-supply:${launcher_network_url}")',yaml)
  source=(ROOT/'components/pax_loader/pax_loader.cpp').read_text();worker=source[source.index('void PaxLoader::pax_load_task_entry_'):source.index('void PaxLoader::pax_catalog_task_entry_')]
  self.assertLess(worker.index('pax_supply::resolve'),worker.index('sync_app_data_'))
  for name in ('elecrow','waveshare'):
   self.assertNotIn('dev-builds/launcher_ui.pax',(ROOT/f'esp32-p4-{name}-pax-web.yaml').read_text())
if __name__=='__main__':unittest.main()
