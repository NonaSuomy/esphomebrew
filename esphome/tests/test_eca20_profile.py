"""Run with the PAX tool environment to validate the active external DSI driver."""
from pathlib import Path
import unittest

from esphome.config import read_config
from esphome.core import CORE
from esphome import config_validation as cv

ROOT = Path(__file__).resolve().parents[1]


class Eca20ProfileTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        CORE.config_path = ROOT / "esp32-p4-nano-pax-3button.yaml"
        config = read_config({})
        if config is None:
            raise RuntimeError("NANO configuration is invalid")
        from esphome.components.mipi_dsi.display import CONFIG_SCHEMA
        cls.schema = staticmethod(CONFIG_SCHEMA)
        cls.display = config["display"][0]

    def test_pax_preserves_565_framebuffer_and_888_output(self):
        self.assertEqual(self.display["model"], "ECA20-MHA1-I")
        self.assertEqual(self.display["color_depth"], "16")
        self.assertEqual(self.display["pixel_mode"], "24bit")
        self.assertTrue(self.display["continuous_clock"])
        self.assertAlmostEqual(self.display["pclk_frequency"], 74250000)

    def test_standalone_defaults(self):
        result = self.schema({"model": "ECA20-MHA1-I"})
        self.assertEqual(result["color_depth"], "24")
        self.assertTrue(result["video_mode_only"])
        self.assertTrue(result["continuous_clock"])
        self.assertNotIn("reset_pin", result)
        self.assertNotIn("enable_pin", result)

    def test_rejects_invalid_bridge_configuration(self):
        for override in (
            {"init_sequence": [[0x11]]},
            {"invert_colors": True},
            {"continuous_clock": False},
            {"video_mode_only": False},
            {"pixel_mode": "16bit"},
            {"lanes": 1},
            {"color_order": "BGR"},
            {"transform": {"mirror_x": True, "mirror_y": False}},
        ):
            with self.subTest(override=override), self.assertRaises(cv.Invalid):
                self.schema({"model": "ECA20-MHA1-I", **override})


if __name__ == "__main__":
    unittest.main()
