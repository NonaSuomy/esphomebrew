"""Real POSIX directory paging, including names beyond an 8 KiB listing."""
from pathlib import Path
import os,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class DirectoryPages(unittest.TestCase):
 def test_pages_do_not_skip_entries_or_keep_descriptors(self):
  source=(ROOT/'components/pax_loader/pax_loader.cpp').read_text()
  method=source[source.index('static int list_app_directory_page('):source.index('int PaxLoader::svc_file_list_dir(')]
  code=r'''
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <set>
#include <dirent.h>
#include <sys/stat.h>
#include "pax_directory_page.h"
'''+method+r'''
static int descriptors(){DIR *d=opendir("/proc/self/fd");assert(d);int n=0;while(readdir(d))n++;closedir(d);return n;}
int main(int argc,char **argv){
 assert(argc==2);std::string folder=argv[1];std::set<std::string> found;
 uint32_t cursor=0;char buffer[512];int pages=0;
 do{
  int count=list_app_directory_page(folder,cursor,buffer,sizeof(buffer));assert(count>0 && count<=32);
  pax_directory_page page;std::memcpy(&page,buffer,sizeof(page));assert(page.magic==PAX_DIRECTORY_MAGIC);
  assert(page.count==(unsigned)count && page.next_offset==cursor+count && page.bytes<=sizeof(buffer)-sizeof(page));
  size_t pos=sizeof(page);for(int i=0;i<count;i++){std::string name=buffer+pos;pos+=name.size()+1;assert(found.insert(name).second);}
  pages++;if(!page.more)break;cursor=page.next_offset;
 }while(pages<1000);
 assert(found.size()==701 && found.count("folder/") && pages>32);
 assert(list_app_directory_page(folder+"/missing",0,buffer,sizeof(buffer))==-1);
 assert(list_app_directory_page(folder,0,buffer,20)==-1);
 int before=descriptors();
 for(int i=0;i<2000;i++)assert(list_app_directory_page(folder,0,buffer,sizeof(buffer))>0);
 assert(descriptors()==before);
 assert(list_app_directory_page(folder,10000,buffer,sizeof(buffer))==0);
}
'''
  with tempfile.TemporaryDirectory() as td:
   p=Path(td);folder=p/'entries';folder.mkdir();(folder/'folder').mkdir()
   for i in range(700):(folder/f'long-package-name-abcdefghijklmnopqrstuvwxyz-{i:04}.pax').touch()
   (p/'test.cpp').write_text(code)
   subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-I',str(ROOT/'components/pax_loader'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test'),str(folder)],check=True,timeout=15,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
if __name__=='__main__':unittest.main()
