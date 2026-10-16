from pathlib import Path
import json,re,hashlib,argparse,importlib.util
from esphome import yaml_util
from esphome.core import CORE
from esphome.components.packages import do_packages_pass,merge_packages
from esphome.components.substitutions import do_substitution_pass
E=Path(__file__).resolve().parent.parent
# This script is installed under pax-esphome/tests.
if not (E/'profiles.json').is_file():E=Path('/home/nonasuomy/code/pax/pax-esphome')

def resolved(path):
 CORE.reset();CORE.config_path=path
 return merge_packages(do_substitution_pass(do_packages_pass(yaml_util.load_yaml(path))))

def normalized(value,key=''):
 if isinstance(value,dict):return {k:normalized(v,k) for k,v in value.items() if k!='substitutions'}
 if isinstance(value,list):
  out=[normalized(v) for v in value]
  return out
 if key in {'width','height','x','y','canvas_width','canvas_height','panel_width','panel_height'} and isinstance(value,str) and value.isdigit():return int(value)
 if not isinstance(value,(str,int,float,bool,type(None))):return str(value)
 return value

def digest(value):return hashlib.sha256(json.dumps(normalized(value),sort_keys=True).encode()).hexdigest()

parser=argparse.ArgumentParser();parser.add_argument('--baseline',type=Path);parser.add_argument('--report',type=Path);args=parser.parse_args()
profiles=json.loads((E/'profiles.json').read_text());report={'profiles':[],'preserved_existing_profiles':[]}
for key,file in profiles.items():
 path=E/file;c=resolved(path);n=normalized(c)
 if 'host' in c:
  assert n['display'][0]['platform']=='sdl' and n['touchscreen'][0]['platform']=='sdl',key
  assert not any(k in c for k in ('esp32','pax_loader','sd_mmc_card','pax_web')),key
  assert n['pax_desktop']['id']=='pax_runtime',key
  report['profiles'].append({'key':key,'yaml':file,'target':'host','resolved_sha256':digest(c)})
  continue
 loader=n['pax_loader'];sd=n['sd_mmc_card']
 assert loader['canvas_width']<=loader['panel_width'] and loader['canvas_height']<=loader['panel_height'],key
 assert sd['id']==loader['sd_mmc_id']=='sd_card',key
 if c.get('esp32_hosted',{}).get('type','sdio')=='sdio' and 'esp32_hosted' in c:
  assert sd['slot']!=c['esp32_hosted']['slot'],key
 assert c['pax_web']['port']==8080 and c['improv_serial']['next_url']=='http://{{ip_address}}:8080/pax',key
 if 'hdmi' in file or key in ['nano','nano1080','nano-wifi']:
  assert not c.get('touchscreen') and c['lvgl']['touchscreens']==[],key
  assert c['display'][0]['video_mode_only'],key
 if key=='nano-lcd101':
  assert n['display'][0]['model']=='WAVESHARE-P4-NANO-10.1',key
  assert n['display'][0]['rotation']==90 and loader['panel_width']==1280 and loader['panel_height']==800,key
  assert n['touchscreen'][0]['transform']=={'swap_xy':True,'mirror_y':True},key
  assert n['i2c_device'][0]['address']==0x45 and n['sd_mmc_card']['slot']==0,key
 # Every button fits its physical screen and has a usable hit target.
 for widget in n['lvgl']['pages'][0]['widgets']:
  if 'button' in widget:
   b=widget['button'];assert 0<=b['x'] and b['x']+b['width']<=loader['panel_width'],key
   assert 0<=b['y'] and b['y']+b['height']<=loader['panel_height'] and b['height']>=100,key
 record={'key':key,'yaml':file,'resolved_sha256':digest(c)}
 if args.baseline and (args.baseline/(file+'.json')).is_file():
  before=merge_packages(json.loads((args.baseline/(file+'.json')).read_text()))
  assert normalized(before)==n,'Resolved behavior changed for '+key
  report['preserved_existing_profiles'].append(key)
 report['profiles'].append(record)
# Check smaller-panel bounds without reducing the established legacy defaults.
from esphome.config import load_config
CORE.reset();CORE.config_path=E/profiles["jc4880"]
validated=load_config({});assert not validated.errors
from esphome.components.pax_loader import even_pixels,validate_canvas,CANVAS_MIN_WIDTH,CANVAS_MIN_HEIGHT,LEGACY_CANVAS
assert LEGACY_CANVAS==(800,480)
assert even_pixels(CANVAS_MIN_WIDTH)(480)==480 and even_pixels(CANVAS_MIN_WIDTH)(720)==720
try:even_pixels(CANVAS_MIN_WIDTH)(479)
except Exception:pass
else:raise AssertionError('Odd panel dimensions accepted')
desktop_count=sum(item.get("target")=="host" for item in report["profiles"])
print(f"Checked {len(profiles)-desktop_count} board and {desktop_count} desktop profiles, services and button bounds; preserved {len(report['preserved_existing_profiles'])} existing configurations")
if args.report:args.report.write_text(json.dumps(report,indent=2)+'\n')
