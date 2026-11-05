"""POSIX iconv tests do not certify Windows API behavior."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest
from prepare_encoding_fixtures import prepare

ROOT = Path(__file__).resolve().parents[1]
RT = ROOT / 'third_party/src/PS2Recomp/ps2xRuntime'
IOP = ROOT / 'third_party/src/PS2Recomp/ps2xIOP'


@unittest.skipUnless(shutil.which('g++'), 'encoding fixtures require g++')
class VfsEncodingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        path = Path(cls.directory.name)
        prepare(path)
        cls.exe = path / 'vfs_encoding'
        command = ['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-fsanitize=address,undefined',
                   '-fno-sanitize-recover=all', '-no-pie', '-I', str(RT / 'include'), '-I', str(IOP / 'include'),
                   '-I', str(ROOT / 'examples/generic_runner'), '-I', str(path),
                   str(ROOT / 'tests/vfs_encoding_tests.cpp'), str(RT / 'src/lib/ps2_vfs.cpp'),
                   str(RT / 'src/lib/ps2_rom_device.cpp'), str(IOP / 'src/ps2_path.cpp'), '-o', str(cls.exe)]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr)

    def run_case(self, mode, encoding):
        with tempfile.TemporaryDirectory(prefix='vfs-encoding-') as root:
            result = subprocess.run([str(self.exe), mode, encoding, root], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)


def add_case(mode, encoding):
    def test(self):
        self.run_case(mode, encoding)
    setattr(VfsEncodingTests, 'test_' + encoding + '_' + mode.replace('-', '_'), test)


for encoding in ('utf8', 'sjis', 'gbk', 'big5'):
    for mode in ('read', 'create', 'save', 'trail', 'decode-invalid'):
        add_case(mode, encoding)
for encoding in ('sjis', 'gbk', 'big5'):
    add_case('raw-reject', encoding)
for encoding in ('gbk', 'big5'):
    add_case('ambiguous', encoding)
add_case('utf8-invalid', 'utf8')
add_case('switches', 'utf8')


@unittest.skipUnless(os.name == 'nt', 'native Windows drive paths require Windows CI')
def test_native(self):
    self.run_case('native', 'utf8')


VfsEncodingTests.test_windows_native = test_native
