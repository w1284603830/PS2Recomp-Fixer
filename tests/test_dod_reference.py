"""Production reference configuration + pack preparation; no game or GPU resources."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import zlib
from test_particle_edges import function
ROOT=Path(__file__).resolve().parents[1]
REF=ROOT/'examples/dod_runner'
GS=ROOT/'third_party/src/pcsx2gs/pcsx2/pcsx2'

@unittest.skipUnless(shutil.which('g++'),'reference C++ fixtures require g++')
class DodReferenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.temp.cleanup);cls.exe=Path(cls.temp.name)/'dod-reference-tests'
        p=subprocess.run(['g++','-std=c++20','-O1','-g','-Wall','-Wextra','-fsanitize=undefined','-fno-sanitize-recover=all','-I',str(REF),'-I',str(GS),str(ROOT/'tests/dod_reference_tests.cpp'),'-o',str(cls.exe)],capture_output=True,text=True)
        if p.returncode:raise RuntimeError(p.stderr)
        main=(REF/'main.cpp').read_text();directory=Path(cls.temp.name)
        (directory/'dod_runner_helpers_current.inc').write_text(function(main,'    std::string trim(')+function(main,'    std::map<std::string, std::string> readIni(')+function(main,'    fs::path resolveFrom('))
        start=main.index('        // Optional reference rendering is preflighted')
        end=main.index('        // The game reads its data sectors',start)
        (directory/'dod_runner_preflight_current.inc').write_text(main[start:end])
        cls.preflight=directory/'dod-preflight-tests'
        p=subprocess.run(['g++','-std=c++20','-O1','-g','-Wall','-Wextra','-fsanitize=undefined','-fno-sanitize-recover=all','-I',str(REF),'-I',str(directory),str(ROOT/'tests/dod_preflight_tests.cpp'),'-o',str(cls.preflight)],capture_output=True,text=True)
        if p.returncode:raise RuntimeError(p.stderr)
    def run_case(self,case,*args,extra=None):
        env={k:v for k,v in os.environ.items() if k not in ('PS2_GS_OPTIONS','DOD_GS_OPTIONS','PS2_UI_HD_DIR','DOD_UI_HD_DIR')};env.update(extra or {})
        p=subprocess.run([str(self.exe),case,*map(str,args)],env=env,capture_output=True,text=True,timeout=10)
        self.assertEqual(p.returncode,0,p.stderr)
    def test_default_retains_gameindex_without_selecting_enhancements(self):self.run_case('default')
    def test_explicit_reference_and_force_off(self):self.run_case('particle')
    def test_missing_wrong_revision_and_crc_fail_closed(self):self.run_case('unconfirmed')
    def test_raw_particle_options_need_confirmation(self):self.run_case('raw')
    def test_duplicate_conflicting_and_other_game_options_rejected(self):self.run_case('conflicts')
    def test_hd_ui_and_scaling_are_explicit_and_independent_of_particles(self):self.run_case('hd-ui')
    def test_ini_then_environment_override_order_is_preserved(self):self.run_case('precedence')
    def test_invalid_flags_and_crc_syntax(self):self.run_case('invalid')
    def test_repeat_configuration_does_not_inherit_prior_selection(self):self.run_case('reuse')
    def test_incomplete_arguments_fail_before_partial_overrides(self):self.run_case('arguments')
    def test_public_env_precedes_legacy_before_runtime_construction(self):self.run_case('environment',extra={'PS2_GS_OPTIONS':'public','DOD_GS_OPTIONS':'legacy','PS2_UI_HD_DIR':'public-ui','DOD_UI_HD_DIR':'legacy-ui'})
    def test_elf_crc_matches_independent_zlib_and_multiple_chunks(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'renamed.elf';data=bytearray(52);data[:7]=b'\x7fELF\x01\x01\x01';data[16]=2;data[18]=8;data+=bytes(range(256))*70;p.write_bytes(data)
            self.run_case('elf-crc',p,str(zlib.crc32(data)))
    def test_missing_truncated_and_wrong_arch_elf_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'SLPM_662.75';self.run_case('elf-reject',p)
            for data in (b'',b'ELF',bytes(52),b'\x7fELF\x02\x01\x01'+bytes(45)):
                p.write_bytes(data);self.run_case('elf-reject',p)

    def run_preflight(self,mode,extra=None):
        with tempfile.TemporaryDirectory() as d:
            env={k:v for k,v in os.environ.items() if k not in ('PS2_GS_OPTIONS','DOD_GS_OPTIONS','PS2_UI_HD_DIR','DOD_UI_HD_DIR')};env.update(extra or {})
            p=subprocess.run([str(self.preflight),mode,d],env=env,capture_output=True,text=True,timeout=10)
            self.assertEqual(p.returncode,0,p.stderr)
    def test_actual_preflight_rejects_before_window_initialization(self):self.run_preflight('refusal')
    def test_actual_ini_and_preflight_accept_only_explicit_assets(self):self.run_preflight('ini-success')
    def test_actual_environment_request_cannot_bypass_confirmation(self):self.run_preflight('environment-refusal',{'PS2_GS_OPTIONS':'game_id=onimusha-dawn-of-dreams;game_revision=SLPM_662.75;particle_edges_profile=dod-slpm66275-native-grid-v1'})

class DodPackPreparationTests(unittest.TestCase):
    def run_tool(self,pack,*extra):
        return subprocess.run([sys.executable,str(REF/'prepare_texture_pack.py'),'--pack',str(pack),'--confirmed-game-revision','SLPM_662.75','--confirmed-pack','DoD_HD_CN-reference',*extra],capture_output=True,text=True,timeout=10)
    def test_dry_run_and_explicit_apply_leave_textures_unchanged(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);texture=p/'example.dds';texture.write_bytes(b'synthetic-placeholder');before=texture.read_bytes()
            result=self.run_tool(p);self.assertEqual(result.returncode,0,result.stderr);self.assertFalse((p/'texture_exclusions.txt').exists())
            result=self.run_tool(p,'--apply');self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual((p/'texture_exclusions.txt').read_bytes(),(REF/'texture_exclusions.txt').read_bytes());self.assertEqual(texture.read_bytes(),before)
    def test_existing_different_manifest_is_preserved(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);(p/'texture_exclusions.txt').write_text('my-own.dds\n');result=self.run_tool(p,'--apply')
            self.assertNotEqual(result.returncode,0);self.assertEqual((p/'texture_exclusions.txt').read_text(),'my-own.dds\n')
    def test_same_manifest_is_idempotent_and_missing_pack_is_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);self.assertNotEqual(self.run_tool(p/'missing','--apply').returncode,0);shutil.copyfile(REF/'texture_exclusions.txt',p/'texture_exclusions.txt')
            result=self.run_tool(p,'--apply');self.assertEqual(result.returncode,0,result.stderr);self.assertIn('already present',result.stdout)
