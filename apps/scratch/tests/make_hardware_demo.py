#!/usr/bin/env python3
"""Extend the original PAX smoke demo with interactive Elecrow hardware examples.

Only sensor reads run automatically. Actuators run from touch/keyboard hats.
Generate with: python3 tests/make_hardware_demo.py
"""
import copy
import hashlib
import html
import io
import math
import struct
import wave
import json
from pathlib import Path
import sys
import zipfile

ROOT = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent
with zipfile.ZipFile(ROOT / 'data/PAX Demo.sb3') as z:
    assets = {n: z.read(n) for n in z.namelist() if n != 'project.json'}
    project = json.loads(z.read('project.json'))
stage = project['targets'][0]
stage['variables'].update({
    'hw_page': ['Page', 1], 'hw_title': ['Hardware page', 'Environment'],
    'hw_action': ['Action', 'Tap a button or use Space / 1..8'],
    'hw_busy': ['IO busy', 0], 'hw_request': ['Requested example', 0], 'hw_brightness': ['Saved brightness', 80],
    'hw_speaker': ['Saved speaker enable', 1], 'hw_mic': ['Saved microphone enable', 0],
})
stage['lists']['hw_readings'] = ['Board readings', ['Starting...'] + [''] * 7]

# Nodes are authored as native Scratch block graphs, not a runtime plugin.
def node(op, inputs=None, fields=None):
    return (op, inputs or {}, fields or {})
def var(ident):
    return node('data_variable', fields={'VARIABLE': [stage['variables'][ident][0], ident]})
def op(name, a, b):
    return node('operator_' + name, {'NUM1' if name in ('add', 'subtract', 'multiply', 'divide', 'mod') else 'OPERAND1': a,
                                   'NUM2' if name in ('add', 'subtract', 'multiply', 'divide', 'mod') else 'OPERAND2': b})
def join(a, b):
    return node('operator_join', {'STRING1': a, 'STRING2': b})
def read(channel):
    return node('paxio_read', {'CHANNEL': channel})
def available(channel):
    return node('paxio_available', {'CHANNEL': channel})
def rounded(value, places=1):
    factor = 10 ** places
    return op('divide', node('operator_round', {'NUM': op('multiply', value, factor)}), factor)
def setvar(ident, value):
    return node('data_setvariableto', {'VALUE': value}, {'VARIABLE': [stage['variables'][ident][0], ident]})
def status(message):
    return setvar('hw_action', message)
def wait(seconds):
    return node('control_wait', {'DURATION': seconds})
def branch(condition, yes, no=None):
    return node('control_if_else' if no is not None else 'control_if',
                {'CONDITION': condition, 'SUBSTACK': yes, **({'SUBSTACK2': no} if no is not None else {})})
def write(channel, value):
    return node('paxio_write', {'CHANNEL': channel, 'VALUE': value})
def replace(index, value):
    return node('data_replaceitemoflist', {'INDEX': index, 'ITEM': value}, {'LIST': ['Board readings', 'hw_readings']})

class Graph:
    def __init__(self, prefix):
        self.blocks, self.prefix, self.counter = {}, prefix, 0
    def add(self, spec, parent=None, top=False, xy=(30,30)):
        self.counter += 1
        ident = self.prefix + str(self.counter)
        opcode, inputs, fields = spec
        b = dict(opcode=opcode, next=None, parent=parent, inputs={}, fields=fields,
                 shadow=opcode.endswith('_menu'), topLevel=top)
        if top: b.update(x=xy[0], y=xy[1])
        self.blocks[ident] = b
        for name, value in inputs.items():
            if isinstance(value, list):
                child = self.chain(value, ident)
                if child: b['inputs'][name] = [2, child]
            elif isinstance(value, tuple):
                b['inputs'][name] = [2, self.add(value, ident)]
            else:
                b['inputs'][name] = [1, [4 if isinstance(value, (int, float)) else 10, str(value)]]
        return ident
    def chain(self, specs, parent=None):
        first = previous = None
        for spec in specs:
            ident = self.add(spec, previous or parent)
            if previous: self.blocks[previous]['next'] = ident
            else: first = ident
            previous = ident
        return first
    def script(self, hat, body, xy=(30,30)):
        ident = self.add(hat, top=True, xy=xy)
        self.blocks[ident]['next'] = self.chain(body, ident)
        return ident
    def comment(self, ident, text):
        return dict(blockId=ident, x=30, y=30, width=310, height=140, minimized=False, text=text)

# Each page has eight fixed rows. Replacing avoids ever-growing lists.
button = lambda name: node('paxio_button', fields={'BUTTON': [name, None]})
pages = [
    ('Environment', [('Temperature', 'temperature_c', ' C', 1), ('Humidity', 'humidity_pct', ' %', 1),
                     ('Light', 'light_lux', ' lux', 1), ('Distance', 'distance_cm', ' cm', 1),
                     ('MQ2 raw', 'gas_voltage', ' V', 2), ('PIR motion', 'motion', '', 0)]),
    ('Tilt and rotation', [(f'Acceleration {a.upper()}', 'accel_'+a, ' g', 2) for a in 'xyz'] +
                          [(f'Gyro {a.upper()}', 'gyro_'+a, ' deg/s', 1) for a in 'xyz']),
    ('Buttons and touch', [('Buttons ADC', 'buttons_voltage', ' V', 2),
                          ('Up', button('dpadUp'), '', 0), ('Down', button('dpadDown'), '', 0),
                          ('Left', button('dpadLeft'), '', 0), ('Right', button('dpadRight'), '', 0),
                          ('A / Space', button('A'), '', 0),
                          ('Mouse / touch X', node('sensing_mousex'), '', 0),
                          ('Mouse / touch Y', node('sensing_mousey'), '', 0)]),
    ('More controller inputs', [(n, button(code), '', 0) for n,code in [('B','B'),('X','X'),('Y','Y'),('L','shoulderL'),('R','shoulderR'),('Start','start'),('Select','back')]] +
                               [('Mouse / touch down', node('sensing_mousedown'), '', 0)]),
    ('Optional inputs', [('Hall magnet', 'hall', '', 0), ('Touch pad', 'touch_pad', '', 0),
                         ('Microphone RMS', 'microphone_level', ' %', 1),
                         ('Mic enabled', 'microphone_enabled', '', 0),
                         ('Camera ready', 'camera_ready', '', 0),
                         ('Capture result', 'camera_capture_status', '', 0)]),
    ('RGBW output state', [('Red', 'led_red', ' %', 0), ('Green', 'led_green', ' %', 0),
                           ('Blue', 'led_blue', ' %', 0), ('White', 'led_white', ' %', 0),
                           ('LED brightness', 'led_brightness', ' %', 0)]),
    ('Other output state', [('Relay', 'relay', '', 0), ('Servo', 'servo', ' %', 0),
                           ('LCD brightness', 'display_brightness', ' %', 0),
                           ('Speaker enabled', 'speaker_enabled', '', 0),
                           ('Camera trigger', 'camera_capture', '', 0)]),
]
g = Graph('hw_stage_')
page_scripts = []
for page_number, (title, readings) in enumerate(pages, 1):
    rows = [setvar('hw_title', title)]
    for i, (label, value, suffix, places) in enumerate(readings, 1):
        if isinstance(value, str):
            rows.append(branch(available(value),
                [replace(i, join(label+': ', join(rounded(read(value), places), suffix)))],
                [replace(i, label+': unavailable')]))
        else:
            rows.append(replace(i, join(label+': ', value)))
    for i in range(len(readings)+1, 9): rows.append(replace(i, ''))
    page_scripts.append(branch(op('equals', var('hw_page'), page_number), rows))
root_hat = g.script(node('event_whenflagclicked'),
    [setvar('hw_page', 1), setvar('hw_busy', 0), setvar('hw_request',0), status('Tap buttons or use Space / 1..8'),
     node('control_forever', {'SUBSTACK': page_scripts+[wait(.5)]})])
stage['blocks'].update(g.blocks)
stage['comments']['hardware_readme'] = g.comment(root_hat,
    'Live readings: Space / Next cycles 7 pages. Reads update twice per second, rounded for display. '
    'Unavailable means missing firmware channel or no sample. MQ2 is raw volts; microphone is RMS percent, not dB. '
    'Hall/touch-pad require the non-radio firmware; camera requires the experimental camera firmware. '
    'Output buttons run only when tapped or their keyboard key is pressed. Output state is the requested value, not physical feedback. '
    'Stop halts scripts: stop during an actuator example can interrupt its cleanup; let timed examples finish or use LED/servo/relay reset blocks.')

# Original SVG/PNG/pen/beep examples remain, with the sprite in the clear right pane.
face = project['targets'][1]
face.update(x=160, y=0, size=80)
face['blocks']['say']['inputs']['MESSAGE'] = [1,[10,'Hello PAX! Tap Temp']]
# Keep the face in its pane. Only the Sound button plays audio.
face['blocks']['say']['next']='pen'
face['blocks']['pen']['parent']='say'
del face['blocks']['sound'], face['blocks']['soundmenu']
movement = Graph('hw_movement_')
first = movement.chain([
    branch(op('gt',node('motion_xposition'),210),[node('motion_pointindirection',{'DIRECTION':-90})]),
    branch(op('lt',node('motion_xposition'),120),[node('motion_pointindirection',{'DIRECTION':90})]),
    wait(.03)],'bounce')
face['blocks']['bounce']['next']=first
face['blocks'].update(movement.blocks)
project['targets'][2].update(x=160, y=-60)

# Clear backdrop, with original pen separator and space for the native list.
svg = ('<svg xmlns="http://www.w3.org/2000/svg" width="480" height="360">'
       '<rect width="480" height="360" fill="#182838"/>'
       '<path d="M0 280H480" stroke="#80a0b0" stroke-width="2"/>'
       '<text x="315" y="97" font-family="Sans Serif" font-size="12" fill="white">Tap or press keys</text>'
       '<text x="315" y="116" font-family="Sans Serif" font-size="10" fill="#a9c5df">Space: next page</text>'
       '<text x="315" y="132" font-family="Sans Serif" font-size="10" fill="#a9c5df">1..7: outputs / 8: temp</text></svg>').encode()
def costume(data, name, width, height):
    digest = hashlib.md5(data).hexdigest()
    assets[digest+'.svg'] = data
    return dict(assetId=digest, name=name, md5ext=digest+'.svg', dataFormat='svg',
                bitmapResolution=1, rotationCenterX=width/2, rotationCenterY=height/2)
stage['costumes'] = [costume(svg, 'Hardware dashboard', 480,360)]
# The request worker lives on Stage, so its sound belongs to Stage too.
stage['sounds'] = copy.deepcopy(face['sounds'])
# Original short melody: normalized harmonic tone with soft attack/release.
rate = 22050
samples = []
for frequency, duration in [(523.25,.18),(659.25,.18),(783.99,.18),(1046.50,.32),
                            (783.99,.16),(659.25,.16),(523.25,.38)]:
    count = round(rate * duration)
    for index in range(count):
        envelope = min(1, index / (rate * .005), (count - 1 - index) / (rate * .025))
        phase = 2 * math.pi * frequency * index / rate
        tone = (math.sin(phase) + .2 * math.sin(2 * phase)) / 1.2
        samples.append(round(19660 * envelope * tone))
    samples.extend([0] * round(rate * .03))
buffer = io.BytesIO()
with wave.open(buffer, 'wb') as wav:
    wav.setparams((1, 2, rate, 0, 'NONE', 'not compressed'))
    wav.writeframes(struct.pack('<' + 'h' * len(samples), *samples))
tune = buffer.getvalue()
digest = hashlib.md5(tune).hexdigest()
assets[digest + '.wav'] = tune
stage['sounds'].append(dict(assetId=digest, name='pax-tune', md5ext=digest+'.wav',
                            dataFormat='wav', rate=rate, sampleCount=len(samples)))
stage['volume'] = 85

worker_actions = []

def add_button(name, key, x, y, body, channel=None, explanation=''):
    # Global busy guards serialize all output demos, including repeated clicks.
    graph = Graph('hw_'+name.lower().replace(' ','_')+'_')
    action = body
    if channel:
        action = [branch(available(channel), body, [status(name+': unavailable in this firmware')])]
    if name != 'Next':
        request_number = len(worker_actions)+1
        worker_actions.append(branch(op('equals', var('hw_request'),request_number),
            [setvar('hw_busy',1),setvar('hw_request',0)]+copy.deepcopy(action)+[setvar('hw_busy',0)]))
        action = [branch(node('operator_and',{'OPERAND1':op('equals',var('hw_busy'),0),
                                            'OPERAND2':op('equals',var('hw_request'),0)}),
                         [setvar('hw_request',request_number)])]
    hat = graph.script(node('event_whenthisspriteclicked'), copy.deepcopy(action))
    graph.script(node('event_whenkeypressed', fields={'KEY_OPTION':[key,None]}), copy.deepcopy(action), (420,30))
    label = html.escape(name + (' ['+key+']' if key != 'space' else ' [Space]'))
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="106" height="28">'
           '<rect x="1" y="1" width="104" height="26" rx="5" fill="#855cd6" stroke="#bda7ee"/>'
           f'<text x="53" y="18" text-anchor="middle" font-family="Sans Serif" font-weight="bold" font-size="11" fill="white">{label}</text></svg>').encode()
    target = dict(isStage=False, name=name, variables={}, lists={}, broadcasts={}, blocks=graph.blocks,
        comments={'help_'+name:graph.comment(hat, explanation)}, currentCostume=0,
        costumes=[costume(svg,name,106,28)], sounds=[copy.deepcopy(face['sounds'][0])] if name=='Sound' else [],
        volume=40, visible=True, x=x, y=y, size=100, direction=90, draggable=False,
        rotationStyle="don't rotate", layerOrder=len(project['targets']))
    project['targets'].append(target)

add_button('Next','space',-175,-122,
    [setvar('hw_page',op('add',op('mod',var('hw_page'),len(pages)),1))], explanation='Cycle the seven live hardware pages. Mouse and touch work on the buttons; USB keyboard uses Space / 1..8.')
led = []
for name, values in [('red',(40,0,0,0)),('green',(0,40,0,0)),('blue',(0,0,40,0)),('white',(0,0,0,40))]:
    led += [status('RGBW: '+name), node('paxio_rgbw',dict(zip(('R','G','B','W'), values))),wait(.4)]
led += [node('paxio_rgbw',dict(zip(('R','G','B','W'),(0,0,0,0)))),write('led_brightness',0),status('RGBW cycle finished; LED off')]
add_button('LED','1',-59,-122,led,'led_red','Cycle red, green, blue and white at 40%, then off. See RGBW output page. Writes are queued, not physical confirmation.')
add_button('Relay','2',57,-122,
    [status('Relay: ON for 0.5 seconds'),node('paxio_relay',{'VALUE':1}),wait(.5),node('paxio_relay',{'VALUE':0}),status('Relay pulse finished; OFF')],
    'relay','Brief relay pulse then off. Stop during the wait interrupts cleanup; allow it to finish.')
add_button('Servo','3',173,-122,
    [status('Servo: forward 60% for 1 second'),node('paxio_servo',{'VALUE':60}),wait(1),node('paxio_servo',{'VALUE':0}),wait(.3),status('Servo: reverse 60% for 1 second'),node('paxio_servo',{'VALUE':-60}),wait(1),node('paxio_servo',{'VALUE':0}),status('Servo finished; stopped (0%)')],
    'servo','Continuous servo: +60% forward for one second, neutral pause, -60% reverse for one second, then stop at 0%. Levels are speed/direction, not degrees; positional revisions move instead.')
add_button('Sound','4',-175,-154,
    [setvar('hw_speaker',read('speaker_enabled')),write('speaker_enabled',1),wait(.25),status('Speaker: playing PAX tune'),
     node('sound_playuntildone',{'SOUND_MENU':node('sound_sounds_menu',fields={'SOUND_MENU':['pax-tune',None]})}),
     wait(.4),write('speaker_enabled',var('hw_speaker')),status('Tune finished; speaker setting restored')],
    'speaker_enabled','Enable the amplifier, allow startup, play an original short tune at 85% Scratch volume, allow buffered audio to drain, then restore speaker enable. Master volume must be above zero.')
add_button('Mic','5',-59,-154,
    [setvar('hw_mic',read('microphone_enabled')),node('paxio_microphone',{'VALUE':1}),setvar('hw_page',5),
     status('Microphone: speak or clap for 8 seconds'),wait(8),node('paxio_microphone',{'VALUE':var('hw_mic')}),
     status('Mic demo finished; enable restored')],
    'microphone_enabled','Start PDM microphone, show RMS on Optional inputs page for eight seconds, restore prior enable. No recording or calibrated dB measurement.')
add_button('Display','6',57,-154,
    [setvar('hw_brightness',read('display_brightness')),status('LCD brightness: 40% for 1 second'),
     write('display_brightness',40),wait(1),write('display_brightness',var('hw_brightness')),
     status('LCD brightness restored')],
    'display_brightness','Dim LCD briefly to 40%, then restore its saved brightness. Does not disable ambient auto-brightness if that is enabled.')
add_button('Camera','7',173,-154,
    [branch(op('equals',read('camera_ready'),1),
        [node('paxio_camera'),setvar('hw_page',5),status('Camera queued; see Capture result (1= saved)')],
        [status('Camera pipeline not ready')])],
    'camera_ready','Experimental camera firmware only. Capture a JPEG on SD; camera_capture_status reports 1 saved / -1 failed. A queued command is not a completed capture.')
# Put the speech example on the original animated face, so its bubble has room.
temp_graph = Graph('hw_temp_')
temp_body = [branch(available('temperature_c'),
    [node('looks_say',{'MESSAGE':join('The temp is: ',join(rounded(read('temperature_c')), ' C'))})],
    [node('looks_say',{'MESSAGE':'Temperature unavailable'})])]
hat = temp_graph.script(node('event_whenthisspriteclicked'),copy.deepcopy(temp_body),(700,30))
temp_graph.script(node('event_whenkeypressed',fields={'KEY_OPTION':['8',None]}),copy.deepcopy(temp_body),(700,250))
face['blocks'].update(temp_graph.blocks)
face['comments']['temperature_example']=temp_graph.comment(hat,
    'Tap this sprite or press 8: say join [The temp is: ] (join ((round (temperature * 10)) / 10) [ C]). '
    'One decimal place without fourteen-digit float noise. C uses current ASCII font support; the degree glyph is not yet available. '
    'The player executes standard round blocks even though the editor palette does not yet offer round.')
# A single flag-thread worker executes the requests. Repeated click/key hats
# can restart safely without cancelling a timed output or its cleanup.
worker = Graph('hw_worker_')
worker.script(node('event_whenflagclicked'),
    [wait(.1),node('control_forever',{'SUBSTACK':worker_actions+[wait(.05)]})],(1000,30))
stage['blocks'].update(worker.blocks)
project['monitors'] = [
    dict(id='hw_title',mode='default',opcode='data_variable',params={'VARIABLE':'Hardware page'},spriteName=None,
         value='Environment',width=0,height=0,x=8,y=7,visible=True,sliderMin=0,sliderMax=100,isDiscrete=True),
    dict(id='hw_action',mode='default',opcode='data_variable',params={'VARIABLE':'Action'},spriteName=None,
         value='Tap buttons or use Space / 1..8',width=0,height=0,x=8,y=31,visible=True,sliderMin=0,sliderMax=100,isDiscrete=True),
    dict(id='hw_readings',mode='list',opcode='data_listcontents',params={'LIST':'Board readings'},spriteName=None,
         value=['Starting...']+['']*7,width=298,height=230,x=8,y=52,visible=True),
]
project['meta']['agent'] = 'PAX authored interactive Elecrow hardware demo'
# Built-in extension names are retained. Custom opcodes are supplied by the PAX player.
project['extensions'] = sorted(set(project['extensions']+['paxio']))
# Omit unreferenced old SVGs to keep the archive small.
used = {a['md5ext'] for t in project['targets'] for a in t['costumes']+t['sounds']}
output = ROOT/'data/PAX-Fun-Demo.sb3'
with zipfile.ZipFile(output,'w',compression=zipfile.ZIP_DEFLATED) as z:
    for name,data in [('project.json',json.dumps(project,separators=(',',':')).encode())]+[(n,assets[n]) for n in sorted(used)]:
        info=zipfile.ZipInfo(name,(2026,10,14,0,0,0));info.compress_type=zipfile.ZIP_DEFLATED
        z.writestr(info,data)
settings=output.with_suffix(output.suffix+'.json')
settings.write_text(json.dumps({'settings':{'sb3InRam':False}},indent=2)+'\n')
print(f'{output}: {output.stat().st_size} bytes; {len(project["targets"])} targets; {sum(len(t["blocks"]) for t in project["targets"])} blocks')
