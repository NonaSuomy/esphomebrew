"""Store v2 recipes retain validation without a 4 KiB parser stack frame."""
from pathlib import Path
import subprocess,tempfile,unittest,re
from test_launcher_background_jobs import SOURCE
ROOT=Path(__file__).resolve().parents[1]
def function(name):
 # Ignore braces inside C strings, character literals and comments.
 cleaned=re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*.*?\*/|//[^\n]*',lambda m:' '*len(m[0]),SOURCE,flags=re.S)
 match=re.search(r'static [^;\n]+\b'+name+r'\([^;]*?\)\s*\{',cleaned)
 a=match.start();i=cleaned.index('{',a)+1;depth=1
 while depth:
  if cleaned[i]=='{':depth+=1
  elif cleaned[i]=='}':depth-=1
  i+=1
 return SOURCE[a:i]
class StoreRecipe(unittest.TestCase):
 def test_long_instructions_validation_legacy_recipe_and_bounded_stack(self):
  a=SOURCE.index('typedef struct {\n  char path[PATH_BYTES];');b=SOURCE.index('static extract_recipe_t g_detail_extract_recipe;',a)
  code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define PATH_BYTES 256
#define MAX_EXTRACT_FILES 24
#define MAX_EXTRACT_MEDIA 4
#define MAX_DETAIL_FILES 24
'''+SOURCE[a:b]+r'''
static void detail_add_required_file(const char *path,const char *note,bool optional,bool download){
 (void)note;(void)optional;(void)download;strcpy(g_detail_files[g_detail_file_count++].path,path);
}
'''+ '\n'.join(function(n) for n in ('find_bytes','has_prefix','ends_with','json_string_field','json_uint_field','json_object_end','json_array_start','json_bool_field','extract_relative_path_valid','extract_target_path_valid','extract_archive_dir_valid','extract_char_equal','extract_ends_with_ci','extract_has_char','extract_recipe_parse_object'))+r'''
static extract_recipe_t recipe;static char json[4600],instructions[4100];
int main(void){
 memset(instructions,'x',4095);instructions[4095]=0;
 snprintf(json,sizeof(json),"{\"extract\":{\"version\":2,\"archive_dir\":\"/sd/roms/probe/incoming\",\"instructions\":\"%s\"}}",instructions);
 assert(extract_recipe_parse_object(json,strlen(json),&recipe) && recipe.valid);
 instructions[4095]='x';instructions[4096]=0;snprintf(json,sizeof(json),"{\"extract\":{\"version\":2,\"archive_dir\":\"/sd/roms/probe/incoming\",\"instructions\":\"%s\"}}",instructions);
 assert(extract_recipe_parse_object(json,strlen(json),&recipe) && recipe.valid); /* existing truncation semantics */
 const char *missing="{\"extract\":{\"version\":2,\"archive_dir\":\"/sd/roms/probe\"}}";
 assert(!extract_recipe_parse_object(missing,strlen(missing),&recipe));
 const char *bad="{\"extract\":{\"version\":2,\"archive_dir\":\"/sd/roms/../probe\",\"instructions\":\"ok\"}}";
 assert(!extract_recipe_parse_object(bad,strlen(bad),&recipe));
 const char *legacy="{\"extract\":{\"version\":1,\"archive_dir\":\"/sd/roms/probe/incoming\",\"media\":[{\"format\":\"zip-iso9660\",\"entry\":\"disc.iso\",\"files\":[{\"source\":\"ONE.BIN\",\"target\":\"/sd/roms/probe/one.bin\"}]}]}}";
 assert(extract_recipe_parse_object(legacy,strlen(legacy),&recipe) && recipe.valid && recipe.file_count==1 && g_detail_file_count==1);
}
'''
  with tempfile.TemporaryDirectory() as td:
   p=Path(td);src=p/'recipe.c';src.write_text(code)
   subprocess.run(['cc','-std=gnu11','-O2','-Wframe-larger-than=512','-Werror','-fstack-usage',str(src),'-o',str(p/'recipe')],check=True)
   subprocess.run([str(p/'recipe')],check=True)
