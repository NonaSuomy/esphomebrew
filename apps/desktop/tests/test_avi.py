"""AVI transport: real ffmpeg pipes, finite RIFF, clocks, EOF and malformed inputs."""
import ctypes as C
import struct
import subprocess
import test_relay as base
import unittest

def chunk(tag, data):
    return tag+struct.pack('<I',len(data))+data+b'\0'*(len(data)&1)
def listing(tag, data, endless=False):
    return b'LIST'+struct.pack('<I',0xffffffff if endless else len(data)+4)+tag+data
def avi(jpeg, audio=b'', rate=48000, channels=2, codec=1, endless=False):
    avih=struct.pack('<10I',500000,0,0,0,1,0,2,0,64,48)+bytes(16)
    vh=bytearray(56);vh[:8]=b'vidsMJPG';struct.pack_into('<II',vh,20,1,2)
    vf=struct.pack('<IiiHH4sIIiiI',40,64,48,1,24,b'MJPG',64*48*3,0,0,0,0)
    ah=bytearray(56);ah[:4]=b'auds'
    af=struct.pack('<HHIIHH',codec,channels,rate,rate*channels*2,channels*2,16)
    hdr=chunk(b'avih',avih)+listing(b'strl',chunk(b'strh',vh)+chunk(b'strf',vf))
    hdr+=listing(b'strl',chunk(b'strh',ah)+chunk(b'strf',af))
    body=listing(b'hdrl',hdr)+chunk(b'JUNK',b'odd')+listing(b'movi',chunk(b'00dc',jpeg)+chunk(b'01wb',audio),endless)
    return b'RIFF'+struct.pack('<I',0xffffffff if endless else len(body)+4)+b'AVI '+body

class AviTests(base.RelayTests):
    # Reuse the protocol bridge/fixture, without inheriting duplicate base tests.
    def setUp(self):
        super().setUp()
        for name in ('test_relay_eof','test_relay_stop','test_relay_audio_rate','test_relay_audio_channels'):
            getattr(self.lib,name).argtypes=[C.c_void_p]
        self.lib.test_relay_pause.argtypes=[C.c_void_p,C.c_int]
    def finish(self, at=1000000):
        self.lib.test_relay_eof(self.state)
        self.lib.test_relay_time(self.state,at)
        result=0
        for _ in range(2000):
            result=self.lib.test_relay_step(self.state)
            if result:break
        return result
    def test_avi_pcm_and_normal_end(self):
        pcm=struct.pack('<8h',100,-100,200,-200,300,-300,400,-400)
        self.assertEqual(self.feed(avi(self.jpeg(),pcm)),0)
        self.assertGreater(self.pixels()[0]>>11,28)
        self.assertEqual(self.lib.test_relay_audio_size(self.state),len(pcm))
        self.assertEqual(self.lib.test_relay_audio_rate(self.state),48000)
        self.assertEqual(self.lib.test_relay_audio_channels(self.state),2)
        self.assertEqual(self.finish(),1)
        self.assertTrue(self.output().endswith(b'E\1'))
    def test_avi_endless_and_mono(self):
        self.assertEqual(self.feed(b'SVPM'+avi(self.jpeg(),bytes(16),rate=16000,channels=1,endless=True)),0)
        self.assertEqual(self.lib.test_relay_audio_channels(self.state),1)
        self.assertEqual(self.finish(),1)
    def test_avi_pause_and_stop(self):
        self.feed(avi(self.jpeg(),bytes(16),endless=True))
        self.lib.test_relay_pause(self.state,1)
        self.lib.test_relay_time(self.state,90000000)
        self.assertEqual(self.drain(),0)
        self.lib.test_relay_pause(self.state,0)
        self.assertEqual(self.drain(),0)
        self.lib.test_relay_stop(self.state)
        self.assertEqual(self.finish(90000000),1)
        self.assertTrue(self.output().endswith(b'E\0'))
    def test_avi_truncated_eof(self):
        data=avi(self.jpeg(),bytes(16),endless=True)
        self.feed(data[:-1]);self.assertEqual(self.finish(),-1)
        self.assertIn(b'Truncated',self.lib.test_relay_error(self.state))
    def test_avi_reject_mp3_and_parent_overflow(self):
        self.assertEqual(self.feed(avi(self.jpeg(),bytes(16),codec=85)),-1)
    def test_avi_reject_big_picture(self):
        data=avi(self.jpeg()).replace(struct.pack('<IiiHH4s',40,64,48,1,24,b'MJPG'),struct.pack('<IiiHH4s',40,65,48,1,24,b'MJPG'))
        self.assertEqual(self.feed(bytes(data)),-1)
    def test_avi_reject_parent_overflow(self):
        data=bytearray(avi(self.jpeg()));struct.pack_into('<I',data,4,8)
        self.assertEqual(self.feed(bytes(data)),-1)
    def test_avi_recovers_after_late_frame(self):
        data=avi(self.jpeg('red'),endless=True)
        self.assertEqual(self.feed(data),0)
        self.lib.test_relay_time(self.state,1200000)
        self.feed(chunk(b'00dc',self.jpeg('blue'))+chunk(b'00dc',self.jpeg('green')))
        self.assertGreater(self.pixels()[0]>>11,28) # blue was late and dropped
        self.lib.test_relay_time(self.state,1700000);self.assertEqual(self.drain(),0)
        self.assertGreater((self.pixels()[0]>>5)&63,25)
        self.assertLess(self.pixels()[0]>>11,4)
    def test_real_ffmpeg_pipe(self):
        data=subprocess.check_output(['ffmpeg','-hide_banner','-loglevel','error',
            '-f','lavfi','-i','color=c=red:s=64x48:r=2:d=1',
            '-f','lavfi','-i','sine=frequency=440:sample_rate=48000:duration=1',
            '-map','0:v','-map','1:a','-c:v','mjpeg','-threads','1','-pix_fmt','yuvj420p',
            '-c:a','pcm_s16le','-ac','2','-f','avi','pipe:1'])
        self.assertEqual(self.feed(data),0)
        for ms in range(0,1600,10):
            self.lib.test_relay_time(self.state,ms*1000)
            self.assertEqual(self.drain(),0)
        self.assertEqual(self.lib.test_relay_audio_size(self.state),48000*4)
        self.assertEqual(self.finish(1600000),1)
        self.assertTrue(self.output().endswith(b'E\1'))

def load_tests(loader, tests, pattern):
    return unittest.TestSuite(AviTests(name) for name in loader.getTestCaseNames(AviTests)
                             if name.startswith('test_avi_') or name == 'test_real_ffmpeg_pipe')
