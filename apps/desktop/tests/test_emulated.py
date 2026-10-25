#!/usr/bin/env python3
"""Execute desktop.pax in the PAX RISC-V host against a loopback VNC server.
Usage: python3 test_emulated.py --workspace /path/to/pax --pax /path/to/desktop.pax
"""
import argparse
import json
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import threading


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace', type=Path, required=True)
    parser.add_argument('--pax', type=Path, required=True)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    packets, errors = [], []
    listening = socket.socket()
    listening.bind(('127.0.0.1', 0))
    listening.listen(1)
    listening.settimeout(20)
    port = listening.getsockname()[1]

    def server():
        try:
            with listening.accept()[0] as conn:
                conn.settimeout(15)

                def read(n):
                    b = bytearray()
                    while len(b) < n:
                        part = conn.recv(n-len(b))
                        if not part:
                            raise EOFError()
                        b.extend(part)
                    return bytes(b)

                conn.sendall(b'RFB 003.008\n')
                assert read(12) == b'RFB 003.008\n'
                conn.sendall(b'\x01\x01')
                assert read(1) == b'\x01'
                conn.sendall(bytes(4))
                assert read(1) == b'\x01'
                conn.sendall(struct.pack('>HH', 800, 480)+bytes(16)+struct.pack('>I', 8)+b'Loopback')
                fmt = read(20)
                assert fmt == bytes([0,0,0,0,16,16,0,1,0,31,0,63,0,31,11,5,0,0,0,0])
                enc = read(4)
                assert enc[0] == 2
                read(struct.unpack('>H', enc[2:])[0]*4)
                assert read(10) == bytes([3,0,0,0,0,0,3,32,1,224])
                # Colourful desktop with vertical RGB stripes; raw RGB565.
                row = b''.join(struct.pack('<H', ((x*31//799)<<11) | (((799-x)*63//799)<<5) | 15) for x in range(800))
                conn.sendall(struct.pack('>BBHHHHHi',0,0,1,0,0,800,480,0)+row*480)
                while True:
                    kind = read(1)[0]
                    n = {3:9,4:7,5:5}[kind]
                    packets.append(bytes([kind])+read(n))
        except EOFError:
            pass
        except Exception as error:
            errors.append(repr(error))

    thread = threading.Thread(target=server, daemon=True)
    thread.start()
    with tempfile.TemporaryDirectory(prefix='pax-desktop-emulated-') as tmp:
        out = args.out or Path(tmp)
        out.mkdir(parents=True, exist_ok=True)
        events = out/'input.json'
        events.write_text(json.dumps([
            {'at':0.4,'type':'touch','x':100,'y':230,'down':True},
            {'at':0.6,'type':'touch','x':100,'y':230,'down':False},
            {'at':4,'type':'key','key':ord('a'),'down':True},
            {'at':4.1,'type':'key','key':ord('a'),'down':False},
            {'at':4.3,'type':'mouse','x':500,'y':300,'buttons':1},
            {'at':4.4,'type':'mouse','x':500,'y':300,'buttons':0},
        ]))
        result = subprocess.run([
            str(args.workspace/'pax-tools/python-desktop/bin/python'),
            str(args.workspace/'pax-tools/desktop/host.py'),'emulated',
            '--pax',str(args.pax),'--headless','--seconds','7',
            '--sd-root',str(Path(tmp)/'sd'), '--app-arg', f'127.0.0.1:{port}',
            '--input-script',str(events),'--report',str(out/'report.json'),
            '--screenshot',str(out/'screen.ppm'),
        ], capture_output=True,text=True,timeout=40)
        (out/'host.log').write_text(result.stdout+result.stderr)
        assert result.returncode == 0, result.stderr[-3000:]
        report = json.loads((out/'report.json').read_text())
        assert report['error'] is None, report
        assert report['frame_colors'] > 20, report
        assert report['services'].get('net_tcp_recv',0) > 0, report
        assert 'Connected to Loopback' in result.stderr, result.stderr
        thread.join(timeout=3)
        assert not errors, errors
        assert any(p[:2] == b'\x04\x01' and p[-1] == ord('a') for p in packets), packets
        assert any(p[0] == 5 for p in packets), packets
        print(json.dumps({'mode':'actual RISC-V PAX in emulator', 'frames':report['frames'],
                          'colors':report['frame_colors'],'received_input_packets':len(packets),
                          'network_receive_calls':report['services']['net_tcp_recv']},indent=2))
    listening.close()


if __name__ == '__main__':
    main()
