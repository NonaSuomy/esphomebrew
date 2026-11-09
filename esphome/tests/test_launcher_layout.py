from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = r"""
#include <cassert>
#include "pax_launcher_layout.h"
using esphome::pax_launcher::launcher_layout;
int main() {
  for (auto size : {std::pair<int,int>{800,480}, {1024,600}, {1280,800}, {480,800}, {720,1280}, {720,720}}) {
    const auto [w,h] = size;
    const auto l = launcher_layout(w,h);
    assert(l.width >= 140 && l.height >= 100);
    for (int i=0;i<3;i++) {
      assert(l.x[i]>=0 && l.y[i]>=0);
      assert(l.x[i]+l.width<=w && l.y[i]+l.height<=h);
      if (i) assert(l.x[i]>=l.x[i-1]+l.width || l.y[i]>=l.y[i-1]+l.height);
    }
  }
  const auto old = launcher_layout(1024,600);
  assert(old.width==270 && old.height==150 && old.x[0]==70 && old.x[1]==377 && old.x[2]==684 && old.y[0]==225);
  const auto small = launcher_layout(800,480);
  assert(small.width==210 && small.height==120);
  const auto portrait = launcher_layout(480,800);
  assert(portrait.width==360 && portrait.height==150 && portrait.y[0]==135 && portrait.y[1]==325 && portrait.y[2]==515);
}
"""
with tempfile.TemporaryDirectory() as directory:
    tmp=Path(directory)
    (tmp/'test.cpp').write_text('#include <utility>\n'+source)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(root),str(tmp/'test.cpp'),'-o',str(tmp/'test')],check=True)
    subprocess.run([str(tmp/'test')],check=True)
print('PASS responsive launcher: bounds, hit targets, no overlaps, Elecrow layout preserved')
