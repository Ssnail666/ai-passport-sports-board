#!/usr/bin/env python3
"""Host checks for the 「运动榜」 Chinese font subsets.

These tests do not render anything: they prove that every character the
application draws on screen exists in every committed font size, so a Chinese
label cannot silently degrade into placeholder boxes after a text change. Real
rendering still has to be confirmed on the device.
"""

import importlib.util
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FONT_DIR = ROOT / "assets" / "fonts"
SPEC = importlib.util.spec_from_file_location(
    "gen_sports_fonts", ROOT / "tools" / "gen_sports_fonts.py"
)
assert SPEC and SPEC.loader
GEN = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = GEN
SPEC.loader.exec_module(GEN)


class SportsFontCoverageTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.display_chars = GEN.collect_display_symbols()

    def test_application_has_chinese_text(self) -> None:
        # A vacuous check would pass if the UI lost all Chinese text by accident.
        self.assertGreaterEqual(len(GEN.collect_chars()), 20)

    def test_every_font_covers_every_displayed_character(self) -> None:
        for name, size in GEN.FONTS:
            with self.subTest(font=name):
                source = FONT_DIR / (name + ".c")
                self.assertTrue(source.is_file(), f"missing generated font {name}.c")
                text = source.read_text(encoding="utf-8")
                self.assertIn(f"* Size: {size} px", text)
                self.assertIn(f"const lv_font_t {name} ", text)

                covered = GEN.covered_codepoints(source)
                missing = sorted(
                    char for char in self.display_chars if ord(char) not in covered
                )
                self.assertEqual(
                    missing,
                    [],
                    "{} lacks {} -> {}".format(
                        name,
                        len(missing),
                        " ".join("U+%04X" % ord(char) for char in missing),
                    ),
                )

    def test_check_is_capable_of_failing(self) -> None:
        # Negative case: a character the application never uses must be absent,
        # proving the coverage check above is not passing unconditionally.
        covered = GEN.covered_codepoints(FONT_DIR / "sports_font_16.c")
        self.assertNotIn(ord("龍"), covered)

    def test_inventory_matches_displayed_characters(self) -> None:
        inventory = (FONT_DIR / "sports_font_chars.txt").read_text(encoding="utf-8")
        declared = ""
        for line in inventory.splitlines():
            if line.startswith("non_ascii: "):
                declared = line[len("non_ascii: "):]
        self.assertEqual(declared, "".join(GEN.collect_chars()))

    def test_large_number_font_and_cmake_are_wired(self) -> None:
        defaults = (ROOT / "sdkconfig.defaults").read_text(encoding="utf-8")
        self.assertIn("CONFIG_LV_FONT_MONTSERRAT_40=y", defaults)
        cmake = (ROOT / "main" / "CMakeLists.txt").read_text(encoding="utf-8")
        for name, _ in GEN.FONTS:
            self.assertIn(name + ".c", cmake)


if __name__ == "__main__":
    unittest.main()
