"""Compile production input and diagnostic code with recording host/GPU services."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_particle_edges import function

ROOT=Path(__file__).resolve().parents[1]
RT=ROOT/'third_party/src/PS2Recomp/ps2xRuntime'
GS=ROOT/'third_party/src/pcsx2gs/pcsx2/pcsx2'

@unittest.skipUnless(shutil.which('g++'),'requires g++')
class NeutralBaseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.temp.cleanup)
        cls.directory=Path(cls.temp.name)
        host=(ROOT/'third_party/src/pcsx2gs/pcsx2-gs/compat/HostStubs.cpp').read_text()
        (cls.directory/'png_capture_current.inc').write_text(function(host,'bool GSPng::Save('))
        (cls.directory/'host_identity_current.inc').write_text(''.join(function(host,sig) for sig in (
            'void pcsx2gs::SetTextureFolder(', 'void pcsx2gs::SetGameTitle(',
            'std::string VMManager::GetDiscSerial()', 'std::string VMManager::GetTitle(')))
        for name,extra in {
            'neutral_policy':[RT/'src/lib/gs',ROOT/'third_party/src/kzvu/include'],
            'input_readers':[ROOT/'tests/stubs/input',RT/'src/lib'],
            'png_capture':[],
            'host_identity':[],
            'hd_adapter':[ROOT/'tests/stubs/graphics'],
        }.items():
            cmd=['g++','-std=c++20','-O1','-g','-Wall','-Wextra','-pthread','-fsanitize=undefined,float-cast-overflow','-fno-sanitize-recover=all']
            for path in [*extra,RT/'include',GS,cls.directory]:cmd+=['-I',str(path)]
            cmd+=[str(ROOT/f'tests/{name}_tests.cpp')]
            if name=='input_readers':cmd+=[str(RT/'src/lib/ps2_pad.cpp')]
            cmd+=['-o',str(cls.directory/name)]
            result=subprocess.run(cmd,capture_output=True,text=True)
            if result.returncode:raise RuntimeError(result.stderr)
    def run_case(self,exe,mode,extra=None):
        env={k:v for k,v in os.environ.items() if not k.startswith(('DOD_PAD_','DOD_TEST_','PS2_PAD_','DOD_CG_','PS2_CG_'))}
        env.update(extra or {})
        result=subprocess.run([str(self.directory/exe),mode],env=env,capture_output=True,text=True,timeout=15)
        self.assertEqual(result.returncode,0,result.stderr)
    def test_presentation_phase_and_movie_latch(self):self.run_case('neutral_policy','presentation')
    def test_all_movie_transition_sequences_submit_once(self):self.run_case('neutral_policy','presentation-sequence')
    def test_render_options_default_off_and_reset(self):self.run_case('neutral_policy','render-options')
    def test_pack_owned_exclusions_and_reload(self):self.run_case('neutral_policy','exclusions')
    def test_manifest_size_and_entry_bounds(self):self.run_case('neutral_policy','exclusion-limits')
    def test_diagnostic_frame_address_strict_and_overflow_safe(self):self.run_case('neutral_policy','address')
    def test_cg_trace_never_mutates_registers(self):self.run_case('neutral_policy','trace')
    def test_readers_acknowledge_independently_of_sio2(self):self.run_case('input_readers','readers')
    def test_menu_and_source_changes_discard_stale_events(self):self.run_case('input_readers','capture')
    def test_trainer_panel_captures_input_like_the_menu(self):self.run_case('input_readers','trainer-capture')
    def test_reader_reset_and_invalid_buffers(self):self.run_case('input_readers','reset')
    def test_reader_capacity_and_unregister(self):self.run_case('input_readers','capacity')
    def test_host_and_reader_concurrency(self):self.run_case('input_readers','threads')
    def test_old_autostart_cannot_select_game_buttons(self):self.run_case('input_readers','boot-neutral',{'DOD_TEST_AUTOSTART':'1'})
    def test_boot_navigation_requires_explicit_sequence(self):self.run_case('input_readers','boot-explicit',{'PS2_PAD_BOOT_SEQUENCE':'START','DOD_TEST_AUTOSTART':'100'})
    def test_requested_split_alpha_and_padded_rows(self):self.run_case('png_capture','alpha')
    def test_rgba_r8_and_channel_order(self):self.run_case('png_capture','formats')
    def test_both_png_failures_propagate(self):self.run_case('png_capture','failure')
    def test_png_null_size_and_pitch_rejected(self):self.run_case('png_capture','bounds')

    def test_hd_cg_full_content_match_on_relocated_buffer(self):self.run_case('hd_adapter','cg-match')
    def test_hd_cg_sparse_collision_cannot_replace(self):self.run_case('hd_adapter','cg-collision')
    def test_hd_cg_packet_overflow_and_scale_bounds(self):self.run_case('hd_adapter','cg-bounds')
    def test_static_pages_require_explicit_format_and_root(self):
        result=subprocess.run([str(self.directory/'hd_adapter'),'static-selection',str(self.directory/'pack')],capture_output=True,text=True,timeout=15)
        self.assertEqual(result.returncode,0,result.stderr)

    def test_neutral_title_pack_identity_and_filesystem_root(self):self.run_case('host_identity','identity')

    def test_trace_public_environment_precedence_before_alias_setup(self):
        self.run_case('neutral_policy','trace-config',{'PS2_CG_GS_TRACE':'12:4','DOD_CG_GS_TRACE':'1:9'})
    def test_trace_malformed_window_fails_closed(self):
        for value in ('','1','-1:2','1:+2','1:2junk','1:2:3','1:18446744073709551616'):
            self.run_case('neutral_policy','trace-off',{'PS2_CG_GS_TRACE':value,'DOD_CG_GS_TRACE':'1:9'})

    def test_uniform_cg_cannot_identify_a_framebuffer(self):self.run_case('hd_adapter','cg-uniform')
    def test_ambiguous_native_previews_keep_native_upload(self):self.run_case('hd_adapter','cg-ambiguous')
    def test_static_page_nonfinite_input_rejected_before_conversion(self):self.run_case('hd_adapter','static-nonfinite')
