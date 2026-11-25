"""Run: python tests/camera_resize_test.py -v (C++17 host compiler; no SDK/device)."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest


HERE = Path(__file__).resolve().parent
CPP = HERE.parent / 'entry/src/main/cpp'
VCVARS = Path('C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Auxiliary/Build/vcvars64.bat')


class CameraResizeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory(prefix='ipcam-resize-')
        cls.addClassCleanup(tmp.cleanup)
        work = Path(tmp.name)
        cls.exe = work / ('camera_resize.exe' if os.name == 'nt' else 'camera_resize')
        # API declarations/implementations live in camera_resize_test.cpp. Empty
        # include paths satisfy only hardware SDK includes in the production TU.
        for header in ('hilog/log.h', 'native_window/external_window.h',
                       'multimedia/player_framework/native_avcodec_base.h',
                       'multimedia/player_framework/native_avcapability.h',
                       'multimedia/player_framework/native_avcodec_videoencoder.h',
                       'multimedia/player_framework/native_avformat.h',
                       'ohcamera/camera.h', 'ohcamera/camera_input.h', 'ohcamera/camera_manager.h',
                       'ohcamera/capture_session.h', 'ohcamera/preview_output.h'):
            path = work / header
            path.parent.mkdir(exist_ok=True, parents=True)
            path.touch()
        source = str(HERE / 'camera_resize_test.cpp')
        if os.name == 'nt' and (shutil.which('cl') or VCVARS.exists()):
            args = ['cl', '/nologo', '/std:c++17', '/utf-8', '/EHsc',
                    f'/I{work}', f'/I{CPP}', source, f'/Fe:{cls.exe}']
            if not shutil.which('cl'):
                batch = work / 'build.cmd'
                batch.write_text(f'@call "{VCVARS}" >nul || exit /b 1\n'
                                 + subprocess.list2cmdline(args), encoding='utf-8')
                args = [os.environ['COMSPEC'], '/d', '/c', str(batch)]
        else:
            compiler = shutil.which('c++') or shutil.which('clang++') or shutil.which('g++')
            if not compiler:
                raise RuntimeError('C++17 compiler required')
            args = [compiler, '-std=c++17', '-pthread', '-I', str(work), '-I', str(CPP),
                    source, '-o', str(cls.exe)]
        subprocess.run(args, cwd=work, check=True, timeout=120)

    def test_capture_orientation(self):
        subprocess.run([self.exe, 'capture-orientation'], check=True, timeout=30)

    def test_rotated_osd_hevc(self):
        subprocess.run([self.exe, 'osd-hevc'], check=True, timeout=30)

    def test_camera_settings_contract(self):
        subprocess.run([self.exe, 'camera-settings'], check=True, timeout=30)

    def test_dual_camera_integration(self):
        for mode in ('dual-surface', 'dual-osd'):
            with self.subTest(mode=mode):
                subprocess.run([self.exe, mode], check=True, timeout=30)

    def test_surface_avc(self):
        subprocess.run([self.exe, 'surface-avc'], check=True, timeout=30)


if __name__ == '__main__':
    unittest.main()
