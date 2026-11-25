"""Exercise all capture orientations on real compositor code, with host camera/window fixtures."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from preview_lifetime_test import build_harness


class CaptureOrientationTest(unittest.TestCase):
    def test_rotated_mirrored_dual_pixels_at_both_sinks(self):
        source = Path(__file__).with_suffix('.cpp').read_text(encoding='utf-8')
        with tempfile.TemporaryDirectory(prefix='ipcam-orientation-') as tmp:
            exe = build_harness(Path(tmp), harness=source)
            subprocess.run([str(exe)], cwd=tmp, timeout=30, check=True)


if __name__ == '__main__':
    unittest.main()
