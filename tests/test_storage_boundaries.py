"""Real file/ROM/path and audio TUs, production HLE bodies; synthetic data only."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from prepare_boundary_fixtures import prepare
ROOT=Path(__file__).resolve().parents[1]
RT=ROOT/'third_party/src/PS2Recomp/ps2xRuntime'
IOP=ROOT/'third_party/src/PS2Recomp/ps2xIOP'
VFS_CASES=('create-preserve','create-readonly','missing','permissions','append','truncate','exclusive','exclusive-race','rom-overflow','large-seek','disc-readonly','versions','invalid','mixed','unicode')
MEMORY_CASES=('segments','spans','vu','gs','mc-string','sif-mirror','sif-scratch-invalid','sif-metadata')
VAG_CASES=('header-bounds','complete','partial','rate-empty','transfer-overflow')

@unittest.skipUnless(shutil.which('g++'),'requires g++ with ASan/UBSan')
class StorageBoundaryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.temp.cleanup);cls.directory=Path(cls.temp.name);prepare(cls.directory)
        units={
            'vfs_boundary':[RT/'src/lib/ps2_vfs.cpp',RT/'src/lib/ps2_rom_device.cpp',IOP/'src/ps2_path.cpp'],
            'memory_boundary':[],
            'vag_boundary':[RT/'src/lib/ps2_audio.cpp',RT/'src/lib/ps2_audio_vag.cpp'],
        }
        for name,extra in units.items():
            cmd=['g++','-std=c++20','-g','-O1','-pthread','-fsanitize=address,undefined','-fno-sanitize-recover=all']
            if sys.platform.startswith('linux'):cmd+=['-fno-pie','-no-pie']
            for path in [ROOT/'tests/stubs/audio',RT/'include',IOP/'include',cls.directory]:cmd+=['-I',str(path)]
            cmd+=[str(ROOT/f'tests/{name}_tests.cpp'),*map(str,extra),'-o',str(cls.directory/name)]
            p=subprocess.run(cmd,capture_output=True,text=True)
            if p.returncode:raise RuntimeError(p.stderr)
    def run_case(self,target,case):
        with tempfile.TemporaryDirectory() as directory:
            command=[str(self.directory/target),case]
            if target=='vfs_boundary':command.append(directory)
            p=subprocess.run(command,capture_output=True,text=True,timeout=20,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
            self.assertEqual(p.returncode,0,p.stderr)

# Separate unittest records make the CI inventory expose each exercised boundary.
for target,cases in [('vfs_boundary',VFS_CASES),('memory_boundary',MEMORY_CASES),('vag_boundary',VAG_CASES)]:
    for case in cases:
        def test(self,t=target,c=case):self.run_case(t,c)
        setattr(StorageBoundaryTests,'test_'+target+'_'+case.replace('-','_'),test)
