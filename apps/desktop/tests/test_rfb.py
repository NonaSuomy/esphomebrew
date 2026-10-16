"""Protocol tests use an in-memory server; no P4, network or VNC installation needed.
Run: python3 -m unittest discover -s apps/desktop/tests -v
"""
import ctypes as C
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

APP = Path(__file__).resolve().parents[1]
READ = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_void_p, C.c_size_t)
ALLOC = C.CFUNCTYPE(C.c_void_p, C.c_void_p, C.c_size_t)
FREE = C.CFUNCTYPE(None, C.c_void_p, C.c_void_p)

class IO(C.Structure):
    _fields_ = [('context', C.c_void_p), ('read', READ), ('write', READ), ('alloc', ALLOC), ('free', FREE)]

class RFB(C.Structure):
    _fields_ = [('io', IO), ('pixels', C.POINTER(C.c_uint16)), ('width', C.c_uint), ('height', C.c_uint),
                ('minor', C.c_uint), ('resized', C.c_uint), ('name', C.c_char * 128), ('error', C.c_char_p)]

def server_init(w=4, h=3):
    return struct.pack('>HH', w, h) + bytes(16) + struct.pack('>I', 4) + b'Test'

def rect(x, y, w, h, encoding, payload=b''):
    return struct.pack('>HHHHi', x, y, w, h, encoding) + payload

def update(*rectangles):
    return struct.pack('>BBH', 0, 0, len(rectangles)) + b''.join(rectangles)

class Peer:
    def __init__(self, lib, data, memory_failure=False):
        self.lib, self.data, self.sent, self.blocks = lib, bytearray(data), bytearray(), {}
        self.read = READ(self._read)
        self.write = READ(self._write)
        self.alloc = ALLOC(self._alloc)
        self.free = FREE(self._free)
        self.fail = memory_failure
        self.r = RFB(io=IO(None, self.read, self.write, self.alloc, self.free))

    def _read(self, _, dest, n):
        if len(self.data) < n:
            return -1
        C.memmove(dest, bytes(self.data[:n]), n)
        del self.data[:n]
        return 0

    def _write(self, _, source, n):
        self.sent.extend(C.string_at(source, n))
        return 0

    def _alloc(self, _, n):
        if self.fail:
            return None
        block = C.create_string_buffer(n)
        address = C.addressof(block)
        self.blocks[address] = block
        return address

    def _free(self, _, address):
        del self.blocks[address]

    def start(self, password=b''):
        return self.lib.rfb_start(C.byref(self.r), password)

    def step(self, data):
        self.data.extend(data)
        return self.lib.rfb_update(C.byref(self.r))

    def pixels(self):
        return list(self.r.pixels[:self.r.width * self.r.height])

    def close(self):
        self.lib.rfb_close(C.byref(self.r))
        assert not self.blocks

class ProtocolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        so = Path(cls.temp.name) / 'rfb.so'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                        str(APP / 'rfb.c'), str(APP / 'vnc_auth.c'), '-o', str(so)], check=True)
        cls.lib = C.CDLL(str(so))
        cls.lib.rfb_start.argtypes = [C.POINTER(RFB), C.c_char_p]
        cls.lib.rfb_update.argtypes = [C.POINTER(RFB)]
        cls.lib.rfb_close.argtypes = [C.POINTER(RFB)]
        cls.lib.rfb_keysym.argtypes = [C.c_int]
        cls.lib.rfb_keysym.restype = C.c_uint32
        cls.lib.vnc_response.argtypes = [C.c_char_p, C.c_void_p, C.c_void_p]

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def peer(self, data=None, **kw):
        if data is None:
            data = b'RFB 003.008\n\x01\x01' + bytes(4) + server_init()
        p = Peer(self.lib, data, **kw)
        self.addCleanup(p.close)
        return p

    def ready(self, **kw):
        p = self.peer(**kw)
        self.assertEqual(p.start(), 0, p.r.error)
        return p

    def test_versions_none(self):
        for minor in (3, 7, 8, 889):
            with self.subTest(minor=minor):
                data = f'RFB 003.{minor:03d}\n'.encode()
                data += struct.pack('>I', 1) if minor == 3 else b'\x01\x01'
                if minor >= 8:
                    data += bytes(4)
                p = self.ready(data=data + server_init())
                version = 3 if minor == 3 else 7 if minor == 7 else 8
                self.assertEqual(p.sent[:12], f'RFB 003.{version:03d}\n'.encode())
                self.assertEqual((p.r.width, p.r.height), (4, 3))
                self.assertEqual(p.r.name, b'Test')
                self.assertEqual(p.sent[-10:], bytes([3,0,0,0,0,0,0,4,0,3]))

    def test_des_openssl_vector(self):
        # OpenSSL DES-ECB, reversed-bit key 0e86ceceeef64e26, input 00..0f.
        out = C.create_string_buffer(16)
        self.lib.vnc_response(b'password', bytes(range(16)), out)
        self.assertEqual(out.raw.hex(), 'b866924125c8eebb9debc1db61c538e2')
        other = C.create_string_buffer(16)
        self.lib.vnc_response(b'passwordEXTRA', bytes(range(16)), other)
        self.assertEqual(out.raw, other.raw)

    def test_authentication(self):
        for minor in (3, 7, 8):
            data = f'RFB 003.{minor:03d}\n'.encode()
            data += struct.pack('>I', 2) if minor == 3 else b'\x02\x01\x02'
            data += bytes(range(16)) + bytes(4) + server_init()
            p = self.peer(data)
            self.assertEqual(p.start(b'password'), 0, p.r.error)
            off = 12 if minor == 3 else 13
            self.assertEqual(p.sent[off:off+16].hex(), 'b866924125c8eebb9debc1db61c538e2')

    def test_auth_failure_and_unsupported_security(self):
        for tail, password in [(b'\x01\x02' + bytes(16) + struct.pack('>I', 1), b'bad'),
                               (b'\x01\x02', b''), (b'\x01\x13', b''), (b'\x00', b'')]:
            p = self.peer(b'RFB 003.008\n' + tail)
            self.assertEqual(p.start(password), -1)
            self.assertTrue(p.r.error)

    def test_raw_rgb565_and_overlapping_copy(self):
        p = self.ready()
        pixels = [0xf800, 0x07e0, 0x001f, 0xffff, 4, 5, 6, 7, 8, 9, 10, 11]
        self.assertEqual(p.step(update(rect(0, 0, 4, 3, 0, struct.pack('<12H', *pixels)))), 1)
        self.assertEqual(p.pixels(), pixels)
        self.assertEqual(p.step(update(rect(1, 1, 3, 2, 1, struct.pack('>HH', 0, 0)))), 1)
        self.assertEqual(p.pixels(), pixels[:5] + pixels[:3] + [8] + pixels[4:7])

    def test_hextile_background_and_subrectangles(self):
        p = self.ready()
        # BG blue, FG red, two red subrectangles, second begins at (2,1).
        payload = b'\x0e' + struct.pack('<HHB', 0x001f, 0xf800, 2) + b'\x00\x00\x21\x11'
        self.assertEqual(p.step(update(rect(0, 0, 4, 3, 5, payload))), 1)
        self.assertEqual(p.pixels(), [0xf800,31,31,31,31,31,0xf800,0xf800,31,31,0xf800,0xf800])
        colored = b'\x1a' + struct.pack('<HBH', 0, 1, 0x07e0) + b'\x10\x12'
        self.assertEqual(p.step(update(rect(0, 0, 4, 3, 5, colored))), 1)
        self.assertEqual(p.pixels(), [0,2016,2016,0]*3)

    def test_hextile_raw_and_inherited_colors(self):
        p = self.ready(data=b'RFB 003.003\n'+struct.pack('>I',1)+server_init(32,2))
        payload = b'\x06' + struct.pack('<HH', 2, 3) + b'\x08\x01\x00\x00'
        self.assertEqual(p.step(update(rect(0, 0, 32, 2, 5, payload))), 1)
        self.assertEqual(p.pixels()[16], 3)
        self.assertEqual(p.pixels()[17], 2)
        self.assertEqual(p.step(update(rect(0,0,2,2,5,b'\x01'+struct.pack('<4H',1,2,3,4)))), 1)
        self.assertEqual(p.pixels()[:2], [1,2])

    def test_resize_requests_full_refresh(self):
        p = self.ready()
        self.assertEqual(p.step(update(rect(0, 0, 2, 2, -223))), 1)
        self.assertEqual((p.r.width, p.r.height), (2, 2))
        self.assertEqual(p.sent[-10:], b'\x03\x00\x00\x00\x00\x00\x00\x02\x00\x02')
        self.assertEqual(len(p.blocks), 1)

    def test_invalid_rectangles_and_sizes(self):
        for packet in [rect(3,2,2,1,0), rect(0,0,2,2,1,struct.pack('>HH',3,2)),
                       rect(0,0,4,3,5,b'\x00'), rect(0,0,4,3,5,b'\x0e'+struct.pack('<HH',1,2)+b'\x01\xff\xff'),
                       rect(0,0,4096,4096,-223), rect(0,0,0,1,-223), rect(0,0,1,1,99)]:
            p = self.ready()
            self.assertEqual(p.step(update(packet)), -1)

    def test_truncated_stream_and_allocation_failure(self):
        p = self.peer(b'RFB')
        self.assertEqual(p.start(), -1)
        p = self.peer(memory_failure=True)
        self.assertEqual(p.start(), -1)
        p = self.ready()
        self.assertEqual(p.step(update(rect(0,0,4,3,0,b'\x00'))), -1)

    def test_bell_clipboard_colormap(self):
        p = self.ready()
        self.assertEqual(p.step(b'\x02'), 0)
        self.assertEqual(p.step(b'\x03\x00\x00\x00'+struct.pack('>I',3)+b'abc'), 0)
        self.assertEqual(p.step(b'\x01\x00'+struct.pack('>HH',0,2)+bytes(12)), 0)
        self.assertFalse(p.data)
        self.assertEqual(p.step(b'\x03\x00\x00\x00'+struct.pack('>I',1048577)), -1)

    def test_keyboard_and_pointer_packets(self):
        p = self.ready()
        self.lib.rfb_key(C.byref(p.r), 0xff52, 1)
        self.assertEqual(p.sent[-8:], b'\x04\x01\x00\x00\x00\x00\xff\x52')
        self.lib.rfb_pointer(C.byref(p.r), 99, 99, 5)
        self.assertEqual(p.sent[-6:], b'\x05\x05\x00\x03\x00\x02')
        for key, sym in [(ord('a'),ord('a')), (153,0xff52), (127,0xff08), (135,0xffbe), (146,0xffc9), (133,0xffe3), (0,0)]:
            self.assertEqual(self.lib.rfb_keysym(key),sym)

if __name__ == '__main__':
    unittest.main()
