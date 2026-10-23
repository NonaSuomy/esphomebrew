"""Run the actual completion method with an active final-file overlay."""
from pathlib import Path
import subprocess
import tempfile
import unittest

class TransferProgress(unittest.TestCase):
    def test_end_transfer_clears_progress_before_releasing_and_refreshing(self):
        source=(Path(__file__).resolve().parents[1]/'esphome/components/pax_loader/pax_loader.cpp').read_text()
        start=source.index('void PaxLoader::end_web_transfer() {')
        method=source[start:source.index('\n}\n', start)+2]
        code=r'''
#include <cassert>
#include <cstring>
struct PaxLoader {
 bool web_storage_busy_=true,active=true;int refreshes=0;char status[64]="Downloading final file";
 void set_progress_(bool value,unsigned done,unsigned total,const char *format,const char *text){
  assert(web_storage_busy_ && !value && !done && !total && !strcmp(format,"%s"));active=value;strcpy(status,text);
 }
 void refresh_catalog(){assert(!web_storage_busy_ && !active && !status[0]);refreshes++;}
 void end_web_transfer();
};
'''+method+r'''
int main(){
 PaxLoader loader;loader.end_web_transfer();assert(loader.refreshes==1);
 loader.web_storage_busy_=true;loader.active=true;strcpy(loader.status,"Download failed");
 loader.end_web_transfer();assert(loader.refreshes==2 && !loader.active && !loader.status[0]);
}
'''
        with tempfile.TemporaryDirectory() as td:
            path=Path(td);(path/'test.cpp').write_text(code)
            subprocess.run(['c++','-std=c++17',str(path/'test.cpp'),'-o',str(path/'test')],check=True)
            subprocess.run([str(path/'test')],check=True)

if __name__=='__main__':unittest.main()
