"""Exercise the real OSD renderer and camera-to-encoder compositor, with host SDK stubs."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from preview_lifetime_test import build_harness


class OsdCompositeTest(unittest.TestCase):
    def test_routes_raster_settings_and_encoded_pixels(self):
        drawings = ('bitmap', 'canvas', 'font_collection', 'register_font', 'text_typography', 'types')
        source = Path(__file__).with_suffix('.cpp').read_text(encoding='utf-8')
        with tempfile.TemporaryDirectory(prefix='ipcam-osd-') as tmp:
            exe = build_harness(Path(tmp), harness=source,
                                extra_headers=[f'native_drawing/drawing_{d}.h' for d in drawings])
            subprocess.run([str(exe)], cwd=tmp, timeout=30, check=True)


if __name__ == '__main__':
    unittest.main()
