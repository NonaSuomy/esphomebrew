"""SVP Relay udisp decoding/input tests, including short reads/writes and JPEG pixels."""
import ctypes as C
import io
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from PIL import Image

APP = Path(__file__).resolve().parents[1]
HEADER = struct.Struct('<HBBHHHHI')
def packet(kind, data=b'', x=0, y=0, w=0, h=0):
    return HEADER.pack(0,kind,0,x,y,w,h,len(data)<<10)+data

class RelayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory()
        out=Path(cls.tmp.name)/'relay.so'
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fPIC','-shared',
            str(APP/'relay.c'),str(APP/'relay_jpeg.c'),str(APP/'jpeg/tjpgd.c'),
            str(APP/'tests/relay_bridge.c'),'-o',str(out)],check=True)
        cls.lib=C.CDLL(str(out))
        cls.lib.test_relay_new.argtypes=[C.c_uint,C.c_uint];cls.lib.test_relay_new.restype=C.c_void_p
        cls.lib.test_relay_free.argtypes=[C.c_void_p]
        cls.lib.test_relay_feed.argtypes=[C.c_void_p,C.c_void_p,C.c_size_t]
        cls.lib.test_relay_step.argtypes=[C.c_void_p]
        cls.lib.test_relay_output.argtypes=[C.c_void_p,C.c_void_p,C.c_size_t];cls.lib.test_relay_output.restype=C.c_size_t
        cls.lib.test_relay_error.argtypes=[C.c_void_p];cls.lib.test_relay_error.restype=C.c_char_p
        cls.lib.test_relay_pixels.argtypes=[C.c_void_p];cls.lib.test_relay_pixels.restype=C.POINTER(C.c_uint16)
        cls.lib.test_relay_cursor.argtypes=[C.c_void_p]
        cls.lib.test_relay_time.argtypes=[C.c_void_p,C.c_int64]
        cls.lib.test_relay_audio_size.argtypes=[C.c_void_p];cls.lib.test_relay_yields.argtypes=[C.c_void_p]
        cls.lib.relay_key.argtypes=[C.c_void_p,C.c_int,C.c_int]
        cls.lib.relay_mouse.argtypes=[C.c_void_p,C.c_int,C.c_int,C.c_int,C.c_uint]
        cls.lib.relay_touch.argtypes=[C.c_void_p,C.POINTER(C.c_int),C.POINTER(C.c_int),C.POINTER(C.c_int),C.c_uint]
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def setUp(self):
        self.state=self.lib.test_relay_new(64,48)
        self.addCleanup(self.lib.test_relay_free,self.state)
        self.drain()
    def drain(self, rounds=100):
        result=0
        for _ in range(rounds):
            result=self.lib.test_relay_step(self.state)
            if result<0:break
        return result
    def feed(self,data):
        self.lib.test_relay_feed(self.state,data,len(data));return self.drain()
    def output(self):
        n=self.lib.test_relay_output(self.state,None,0);buf=C.create_string_buffer(n)
        self.lib.test_relay_output(self.state,buf,n);return buf.raw
    def pixels(self):return list(self.lib.test_relay_pixels(self.state)[:64*48])
    @staticmethod
    def jpeg(color='red',size=(64,48),progressive=False):
        out=io.BytesIO();Image.new('RGB',size,color).save(out,'JPEG',quality=95,subsampling=1,progressive=progressive);return out.getvalue()
    def test_greeting_matches_panel_link(self):
        b=self.output()
        self.assertEqual(b[:3],b'C\x10V')
        self.assertEqual(struct.unpack('<HHBI',b[3:12]),(64,48,0,1048576))
        self.assertEqual(b[12:],b'A\x30\x00N\x02L\x30\x00\x30\x00F\x01S\x01')
    def test_jpeg_decoding_and_rectangle(self):
        self.assertEqual(self.feed(packet(3,self.jpeg(),w=64,h=48)),0)
        red=self.pixels()[0];self.assertGreater(red>>11,28);self.assertLess((red>>5)&63,3)
        self.assertEqual(self.feed(packet(3,self.jpeg('blue',(8,8)),x=4,y=5,w=8,h=8)),0)
        self.assertEqual(self.pixels()[0],red)
        self.assertGreater(self.pixels()[5*64+4]&31,28)
        self.assertGreater(self.lib.test_relay_yields(self.state),0)
    def test_jpeg_subsampling_and_grayscale(self):
        for sampling in (0,1,2):
            out=io.BytesIO();Image.new('RGB',(64,48),'red').save(out,'JPEG',subsampling=sampling)
            self.assertEqual(self.feed(packet(3,out.getvalue(),w=64,h=48)),0)
            self.assertGreater(self.pixels()[-1]>>11,28)
        out=io.BytesIO();Image.new('L',(64,48),128).save(out,'JPEG')
        self.assertEqual(self.feed(packet(3,out.getvalue(),w=64,h=48)),0)
        red=self.pixels()[-1]>>11;green=(self.pixels()[-1]>>5)&63;blue=self.pixels()[-1]&31
        self.assertLess(abs(red-blue),2);self.assertLess(abs(green-2*red),3)
    def test_input_subscription_and_wire_layout(self):
        self.lib.relay_key(self.state,ord('a'),1);self.drain();before=len(self.output())
        self.assertEqual(before,26)
        self.feed(packet(0x21,x=1))
        self.lib.relay_key(self.state,ord('a'),1);self.lib.relay_key(self.state,ord('a'),0)
        self.lib.relay_mouse(self.state,-5,12,-1,2)
        ids=(C.c_int*2)(3,4);xs=(C.c_int*2)(12,25);ys=(C.c_int*2)(15,30)
        self.lib.relay_touch(self.state,ids,xs,ys,2);self.drain()
        b=self.output()[before:]
        self.assertEqual(b,
            b'K'+struct.pack('<BHHBBIB',255,7,4,0,1,ord('a'),0)+
            b'X'+struct.pack('<I',ord('a'))+
            b'K'+struct.pack('<BHHBBIB',255,7,4,0,0,ord('a'),0)+
            b'P'+struct.pack('<hhbB',-5,12,-1,2)+
            b'T\x02'+struct.pack('<BHHBHH',3,12,15,4,25,30))
    def test_dashboard_text_and_shortcuts(self):
        self.feed(packet(0x21,x=1))
        before=len(self.output())
        for key in (' ', 'A', '!'):
            self.lib.relay_key(self.state,ord(key),1)
            self.lib.relay_key(self.state,ord(key),0)
        self.drain();b=self.output()[before:]
        self.assertEqual(len(b),3*31)
        for index,key in enumerate((' ', 'A', '!')):
            self.assertEqual(b[index*31+13:index*31+18],b'X'+struct.pack('<I',ord(key)))
        for modifier in (132,133): # Alt and Ctrl shortcuts must not insert text.
            self.lib.relay_key(self.state,modifier,1)
            self.lib.relay_key(self.state,ord('a'),1)
            self.lib.relay_key(self.state,ord('a'),0)
            self.lib.relay_key(self.state,modifier,0)
        self.drain();self.assertEqual(len(self.output())-before-len(b),8*13)
    def test_special_keys_modifiers(self):
        self.feed(packet(0x21,x=1));self.lib.relay_key(self.state,133,1);self.lib.relay_key(self.state,153,1);self.drain()
        b=self.output()[26:]
        self.assertEqual(struct.unpack('<H',b[4:6])[0],0xe0)
        self.assertEqual(b[6],2)
        self.assertEqual(struct.unpack('<H',b[17:19])[0],0x52)
        self.assertEqual(b[19],2)
    def test_cursor_pointer_and_clipping(self):
        shape=bytes([0,0,255,255,0,255,0,255,255,0,0,255,255,255,255,0])
        self.feed(packet(0x23,shape,x=1,y=1,w=2,h=2)+packet(0x24,x=1,y=1))
        self.lib.test_relay_cursor(self.state)
        self.assertEqual(self.pixels()[:2],[0xf800,0x07e0]);self.assertEqual(self.pixels()[64],0x001f)
        self.feed(packet(0x24,x=0,y=0));self.lib.test_relay_cursor(self.state)
    def test_echo_stats_fill_and_heartbeat(self):
        self.feed(packet(0x22,x=123)+packet(0x20,x=1)+packet(0x7e,bytes(256*1024))+packet(0))
        self.drain(3000);self.lib.test_relay_time(self.state,1000001);self.drain()
        b=self.output()[26:];self.assertEqual(b[:3],b'R\x7b\x00');self.assertEqual(b[3:5],b'M\x0b')
        values=struct.unpack('<11I',b[5:49]);self.assertEqual(values[0],1000);self.assertGreaterEqual(values[1],256*1024)
    def test_pcm(self):
        self.assertEqual(self.feed(packet(0x10,struct.pack('<4h',1,-1,20,-20),w=2,h=48000)),0)
        self.assertEqual(self.lib.test_relay_audio_size(self.state),8)
    def test_reject_bad_rectangles_jpeg_and_formats(self):
        # Each error has its own state, so failure cannot desynchronise/reuse a peer.
        for data in [packet(3,self.jpeg(),x=63,w=64,h=48),packet(3,b'not jpeg',w=64,h=48),
                     packet(3,self.jpeg(size=(8,8)),w=64,h=48),packet(3,self.jpeg(progressive=True),w=64,h=48),
                     packet(0x23,b'bad',w=2,h=2),packet(0x10,b'123',w=2,h=48000),packet(99),
                     HEADER.pack(0,3,0,0,0,64,48,1048577<<10)]:
            self.lib.test_relay_free(self.state);self.state=self.lib.test_relay_new(64,48)
            self.assertEqual(self.feed(data),-1)
            self.assertTrue(self.lib.test_relay_error(self.state))
    def test_silent_sender_timeout(self):
        self.lib.test_relay_time(self.state,30000001);self.assertEqual(self.drain(),0)
        self.lib.test_relay_time(self.state,1800000001);self.assertEqual(self.drain(),-1)
    def test_streaming_sender_timeout_after_stats_reset(self):
        self.feed(packet(0x20,x=1)+packet(3,self.jpeg(),w=64,h=48))
        self.lib.test_relay_time(self.state,1000001);self.assertEqual(self.drain(),0)
        self.lib.test_relay_time(self.state,30000001);self.assertEqual(self.drain(),-1)
    def test_fragmented_headers_payloads_and_backpressure(self):
        data=packet(0x22,x=9)+packet(3,self.jpeg('green'),w=64,h=48)
        for byte in data:
            self.assertEqual(self.feed(bytes([byte])),0)
        self.assertIn(b'R\x09\x00',self.output())
        self.assertGreater((self.pixels()[0]>>5)&63,25)

if __name__=='__main__':unittest.main()
