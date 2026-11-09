"""Check the real SDK ECDH code against independent P-256/P-192 reference results."""
import ctypes
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from cryptography.hazmat.primitives.asymmetric import ec

ESPHOME = Path(__file__).resolve().parents[1]
IDF = Path(os.environ.get('IDF_PATH', ESPHOME.parent / 'pax-tools/idf/frameworks/6.1.0'))
spec = importlib.util.spec_from_file_location('ecc_fix', ESPHOME / 'tools/idf_ecc_compat.py')
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)
PATCH = ESPHOME / 'patches/0008-idf61-tinycrypt-ecc-pairing.patch'

class TinyCryptECCTests(unittest.TestCase):
    def build(self, root, name):
        tiny = root / 'components/bt/common/tinycrypt'
        work = self.temp / name
        (work / 'soc').mkdir(parents=True)
        (work / 'sdkconfig.h').write_text('')
        (work / 'soc/soc_caps.h').write_text('#define SOC_ECC_SUPPORTED 1\n#define SOC_ESP_NIMBLE_CONTROLLER 0\n')
        # The accelerator is unavailable in this host fixture. ECDH must use its
        # regularized software ladder, never truncate its 257-bit scalar into HW.
        (work / 'hardware.c').write_text('''#include <stdint.h>
#include <stdbool.h>
#include "tinycrypt/ecc.h"
int hardware_calls;
int esp_tinycrypt_calc_ecc_mult(const void *x,const void *y,const void *k,void *rx,void *ry,uint8_t n,bool verify){
 (void)x;(void)y;(void)k;(void)rx;(void)ry;(void)n;(void)verify;hardware_calls++;return -1;
}
int esp_tinycrypt_verify_ecc_point(const uint8_t *x,const uint8_t *y,uint8_t n){(void)x;(void)y;(void)n;return -1;}
''')
        lib = work / 'tinycrypt.so'
        subprocess.run(['cc', '-shared', '-fPIC', '-O1', '-DuECC_SUPPORTS_secp192r1=1',
                        '-I'+str(work), '-I'+str(tiny/'include'), '-I'+str(tiny/'port'),
                        str(tiny/'src/ecc.c'), str(tiny/'src/ecc_dh.c'),
                        str(work/'hardware.c'), '-o', str(lib)], check=True)
        loaded = ctypes.CDLL(str(lib))
        for curve in ('uECC_secp256r1', 'uECC_secp192r1'):
            getattr(loaded, curve).restype = ctypes.c_void_p
        loaded.uECC_shared_secret.argtypes = [ctypes.c_void_p]*4
        loaded.uECC_set_rng.argtypes = [ctypes.c_void_p]
        return loaded

    def vectors(self, lib, blinded):
        callback = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_uint)
        @callback
        def rng(dest, size):
            ctypes.memset(dest, 0x42, size)  # fixed test-only blinding value
            return 1
        lib.uECC_set_rng(ctypes.cast(rng, ctypes.c_void_p) if blinded else None)
        cases = 0
        for curve, getter in [(ec.SECP256R1(), lib.uECC_secp256r1), (ec.SECP192R1(), lib.uECC_secp192r1)]:
            count = curve.key_size // 8
            for a, b in [(2, 3), (3, 5), (0x123456789abcdef, 0xfedcba987654321),
                         ((1 << (curve.key_size-1))+17, 0x1234567)]:
                private = ec.derive_private_key(a, curve)
                peer = ec.derive_private_key(b, curve)
                public = peer.public_key().public_numbers()
                encoded = public.x.to_bytes(count, 'big') + public.y.to_bytes(count, 'big')
                output = ctypes.create_string_buffer(count)
                result = lib.uECC_shared_secret(ctypes.create_string_buffer(encoded),
                           ctypes.create_string_buffer(a.to_bytes(count, 'big')), output, getter())
                expected = private.exchange(ec.ECDH(), peer.public_key())
                yield result == 1 and output.raw == expected
                cases += 1
        assert cases == 8

    def test_p4_ecdh_and_idempotent_backport(self):
        if not (IDF/'components/bt/common/tinycrypt').is_dir():
            self.skipTest('ESP-IDF source is not installed')
        with tempfile.TemporaryDirectory() as folder:
            self.temp = Path(folder)
            sdk = self.temp/'sdk'
            shutil.copytree(IDF/'components/bt/common/tinycrypt', sdk/'components/bt/common/tinycrypt')
            helper.patch_idf_ecc(sdk, PATCH)
            self.assertFalse(helper.patch_idf_ecc(sdk, PATCH))
            fixed = self.build(sdk, 'fixed')
            for blinded in (False, True):
                self.assertTrue(all(self.vectors(fixed, blinded)), 'ECDH disagrees with the independent reference')
            self.assertEqual(ctypes.c_int.in_dll(fixed, 'hardware_calls').value, 0)
            # Demonstrate this is a regression test: the original SDK must fail
            # the same vectors when it hands its regularized scalar to hardware.
            subprocess.run(['git', 'apply', '--reverse', '--whitespace=nowarn', str(PATCH)], cwd=sdk, check=True)
            broken = self.build(sdk, 'original')
            self.assertFalse(all(self.vectors(broken, False)))
            self.assertGreater(ctypes.c_int.in_dll(broken, 'hardware_calls').value, 0)
            self.assertTrue(helper.patch_idf_ecc(sdk, PATCH))
            self.assertFalse(helper.patch_idf_ecc(sdk, PATCH))

if __name__ == '__main__':
    unittest.main()
