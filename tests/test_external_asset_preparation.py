"""Private game-data preparation uses only user-supplied bytes."""
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import prepare_anotherworld_data as aw


def table():
    value = bytearray(40)
    value[7] = 1; value[15] = 2; value[19] = 2; value[20] = 255
    return bytes(value)


class PreparationTests(unittest.TestCase):
    def test_original_files_normalize_case_and_validate_bank_ranges(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td); source = root / 'source'; source.mkdir()
            (source / 'MEMLIST.BIN').write_bytes(table())
            (source / 'BANK01').write_bytes(b'ab')
            output = root / 'output'
            self.assertEqual(aw.prepare(source, output), 2)
            self.assertEqual((output / 'memlist.bin').read_bytes(), table())
            self.assertEqual((output / 'bank01').read_bytes(), b'ab')
            (source / 'BANK01').write_bytes(b'a')
            with self.assertRaisesRegex(ValueError, 'truncated bank'):
                aw.prepare(source, output)

    def test_legacy_headers_migrate_without_adding_assets_to_source(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td); source = root / 'source'; source.mkdir()
            data = ','.join(hex(x) for x in table())
            (source / 'datamemlistbin.h').write_text('static const unsigned char gb_memlist_bin[]={' + data + '};')
            (source / 'databank01.h').write_text('static const unsigned char gb_bank01_bin[]={0x61,0x62};')
            self.assertEqual(aw.prepare(source, root / 'output', True), 2)
            self.assertEqual((root / 'output/bank01').read_bytes(), b'ab')

    def test_missing_end_marker_or_resource_table_is_rejected(self):
        with self.assertRaises(ValueError):
            aw.validate({})
        with self.assertRaises(ValueError):
            aw.validate({'memlist.bin': table()[:20], 'bank01': b'ab'})


if __name__ == '__main__':
    unittest.main()
