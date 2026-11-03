"""Compile production particle policy, draw/transform and Bridge lifecycle bodies with recording GS services."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GS = ROOT / "third_party/src/pcsx2gs/pcsx2/pcsx2"
BRIDGE = ROOT / "third_party/src/pcsx2gs/pcsx2-gs"


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    pos = brace + 1
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[start:pos] + "\n"


@unittest.skipUnless(shutil.which("g++"), "particle tests require g++")
class ParticleEdgeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        directory = Path(cls.temp.name)
        renderer = (GS / "GS/Renderers/HW/GSRendererHW.cpp").read_text()
        bridge = (BRIDGE / "compat/Bridge.cpp").read_text()
        (directory / "particle_renderer_current.inc").write_text(
            function(renderer, "const char* GSRendererHW::ApplyParticleEdges(") +
            function(renderer, "void GSRendererHW::ResetParticleEdgeSession()"))
        gate = renderer.index("\tconst ParticleEdges::Profile* particleProfile =")
        (directory / "particle_draw_gate.inc").write_text(renderer[gate:renderer.index("\tif (s_dod_draw_log)", gate)])
        (directory / "particle_bridge_current.inc").write_text(
            function(bridge, "    void ResetParticleSession()") +
            function(bridge, "    int Open(const Pcsx2GsOpenParams *params)") +
            function(bridge, "    void Close()"))
        cls.exe = directory / "particle_tests"
        result = subprocess.run([
            "g++", "-std=c++20", "-O1", "-g", "-Wall", "-Wextra",
            "-fsanitize=undefined", "-fno-sanitize-recover=all", "-I", str(GS),
            "-I", str(BRIDGE / "include"), "-I", str(ROOT / "tests"), "-I", str(directory),
            str(ROOT / "tests/particle_edge_tests.cpp"),
            str(ROOT / "tests/particle_edge_renderer_fixture.cpp"),
            str(ROOT / "tests/particle_edge_bridge_fixture.cpp"), "-o", str(cls.exe),
        ], capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr)

    def run_case(self, case):
        result = subprocess.run([str(self.exe), case], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def test_no_profile_no_mutation_even_with_hpo5(self): self.run_case("default")
    def test_explicit_identity_selects_reference(self): self.run_case("valid")
    def test_invalid_options_fail_closed_without_stale_profile(self): self.run_case("invalid")
    def test_other_game_and_revision_cannot_inherit(self): self.run_case("identity")
    def test_options_are_order_independent_and_duplicates_rejected(self): self.run_case("parsing")
    def test_force_off_and_alias_precedence_per_session(self): self.run_case("env")
    def test_real_bridge_open_close_failed_open_and_ab_reset(self): self.run_case("lifecycle")
    def test_repeated_game_switches_clear_state(self): self.run_case("switches")
    def test_reference_draw_and_vertex_exclusions(self): self.run_case("guards")
    def test_reference_sampling_parameters_and_geometry(self): self.run_case("sampling")
    def test_profile_data_is_independent_and_unsupported_transforms_rejected(self): self.run_case("profile-data")
    def test_production_renderer_matches_frozen_legacy(self): self.run_case("legacy")
    def test_synthetic_session_trace(self):
        self.assertIn("SYNTHETIC ONLY", self.run_case("trace"))
