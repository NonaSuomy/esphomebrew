"""Run the actual loader ZIP extractor on host-generated archives."""
from pathlib import Path
import subprocess
import tempfile
import unittest
import zipfile

ROOT=Path(__file__).resolve().parents[1]
class NamedZipTests(unittest.TestCase):
 def test_named_crc_limit_and_existing_destination(self):
  src=(ROOT/'components/pax_loader/pax_loader.cpp').read_text()
  fragment=src[src.index('static uint16_t zip_u16_'):src.index('void *PaxLoader::svc_file_open',src.index('static uint16_t zip_u16_'))]
  header=r'''
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cstdint>
#include <unistd.h>
#include <zlib.h>
#define inflate(s,f,c) inflate(s,f)
#define ESP_LOGI(...) ((void)0)
#define TAG "test"
using pax_file_progress_fn_t=void (*)(void *,uint64_t,uint64_t);
static std::string app_file_path_(const char *p) { return p?p:""; }
static bool sync_file_(FILE *f,const char *) { return fflush(f)==0; }
static void warn_if_not_durable_(const std::string &) {}
class PaxLoader { public:
static int svc_file_mkdir(const char *) {return 0;}
static int svc_file_zip_extract_first(const char *,const char *,const char *,const char *,char *,size_t);
static int svc_file_zip_extract_first_progress(const char *,const char *,const char *,const char *,char *,size_t,pax_file_progress_fn_t,void *);
static int svc_file_zip_extract_named(const char *,const char *,const char *,uint32_t,uint32_t *);
static int svc_file_zip_extract_first_impl(const char *,const char *,const char *,const char *,char *,size_t,pax_file_progress_fn_t,void *,const char * = nullptr,const char * = nullptr,uint32_t = 0xffffffffU,uint32_t * = nullptr);
};
'''
  main=r'''
#include <cassert>
int main(int argc,char **argv) { assert(argc==5); uint32_t count=0;
 assert(PaxLoader::svc_file_zip_extract_named(argv[1],"nested/disk2.ima",argv[2],100,&count)==0 && count==6);
 assert(PaxLoader::svc_file_zip_extract_named(argv[1],"nested/disk2.ima",argv[2],100,&count)!=0);
 assert(PaxLoader::svc_file_zip_extract_named(argv[1],"nested/disk2.ima",argv[3],5,&count)!=0);
 assert(PaxLoader::svc_file_zip_extract_named(argv[4],"nested/disk2.ima",argv[3],100,&count)!=0);
 assert(access(argv[3],F_OK)!=0); return 0; }
'''
  with tempfile.TemporaryDirectory() as td:
   p=Path(td); archive=p/'good.zip'
   with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as z:
    z.writestr('disk1.ima',b'first');z.writestr('nested/disk2.ima',b'second')
   data=bytearray(archive.read_bytes()); pos=data.index(b'PK\x01\x02'); pos=data.index(b'PK\x01\x02',pos+4);data[pos+16]^=1;(p/'bad.zip').write_bytes(data)
   (p/'test.cpp').write_text(header+fragment+main)
   subprocess.run(['g++','-std=c++17',str(p/'test.cpp'),'-lz','-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test'),str(archive),str(p/'out'),str(p/'no'),str(p/'bad.zip')],check=True)
   self.assertEqual((p/'out').read_bytes(),b'second')
if __name__=='__main__':unittest.main()
