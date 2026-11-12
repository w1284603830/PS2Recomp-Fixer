"""tools/new_port.py on synthetic disc images (no game data): python -m unittest discover -s tests -v."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools/new_port.py"
SECTOR = 2048


def mips_elf(entry=0x00100008, machine=8, cls=1, data=1):
    header = bytearray(52)
    header[:4] = b"\x7fELF"
    header[4], header[5], header[6] = cls, data, 1
    struct.pack_into("<HHIIIIIHHHHHH", header, 16, 2, machine, 1, entry, 0, 0, 0, 52, 32, 0, 40, 0, 0)
    return bytes(header) + b"\0" * 64


def record(name, lba, size, directory=False):
    raw = name.encode("ascii")
    length = 33 + len(raw) + (1 - len(raw) % 2)
    rec = bytearray(length)
    rec[0] = length
    struct.pack_into("<I", rec, 2, lba)
    struct.pack_into(">I", rec, 6, lba)
    struct.pack_into("<I", rec, 10, size)
    struct.pack_into(">I", rec, 14, size)
    rec[25] = 0x02 if directory else 0
    rec[32] = len(raw)
    rec[33:33 + len(raw)] = raw
    return bytes(rec)


def iso_image(files, pvd=True):
    """files: {ISO name: bytes} in the root directory, each in its own extent from sector 21 on."""
    sectors = {}
    entries = [record("\x00", 20, SECTOR, True), record("\x01", 20, SECTOR, True)]
    lba = 21
    for name, content in files.items():
        entries.append(record(name, lba, len(content)))
        sectors[lba] = content
        lba += max(1, (len(content) + SECTOR - 1) // SECTOR)
    image = bytearray(lba * SECTOR)
    if pvd:
        image[16 * SECTOR] = 1
        image[16 * SECTOR + 1:16 * SECTOR + 6] = b"CD001"
        image[16 * SECTOR + 156:16 * SECTOR + 190] = record("\x00", 20, SECTOR, True)
    root = b"".join(entries)
    image[20 * SECTOR:20 * SECTOR + len(root)] = root
    for start, content in sectors.items():
        image[start * SECTOR:start * SECTOR + len(content)] = content
    return bytes(image)


class NewPortTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.dir = Path(self.temp.name)

    def run_tool(self, *args):
        return subprocess.run([sys.executable, "-I", str(TOOL), "--no-analyzer", *args], capture_output=True,
                              text=True, timeout=60)

    def write_iso(self, files, **kwargs):
        iso = self.dir / "disc.iso"
        iso.write_bytes(iso_image(files, **kwargs))
        return iso

    def test_disc_image_to_port_folder(self):
        elf = mips_elf(entry=0x00123450)
        iso = self.write_iso({"SYSTEM.CNF;1": b"BOOT2 = cdrom0:\\SLUS_123.45;1\r\nVER = 1.00\r\nVMODE = NTSC\r\n",
                              "SLUS_123.45;1": elf})
        out = self.dir / "port"
        result = self.run_tool("--iso", str(iso), "--out", str(out), "--title", "Test Game")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((out / "game/SLUS_123.45").read_bytes(), elf)
        info = json.loads((out / "port.json").read_text(encoding="utf-8"))
        self.assertEqual(info["serial"], "SLUS-12345")
        self.assertEqual(info["entry"], 0x00123450)
        self.assertEqual(info["title"], "Test Game")
        toml = (out / "recomp.toml").read_text(encoding="utf-8")
        self.assertIn("native = []", toml)
        self.assertIn((out / "game/SLUS_123.45").as_posix(), toml)
        ini = (out / "game.ini").read_text(encoding="utf-8")
        self.assertIn(f"iso = {iso.resolve()}", ini)
        self.assertIn("title = Test Game", ini)
        steps = (out / "PORT_STEPS.md").read_text(encoding="utf-8")
        for needle in ("ps2_analyzer", "ps2_recomp", "sync_generated.py", "generic_runner", "native"):
            self.assertIn(needle, steps)

    def test_boot_elf_given_directly(self):
        source = self.dir / "SLES_501.23"
        source.write_bytes(mips_elf())
        out = self.dir / "port"
        result = self.run_tool("--elf", str(source), "--out", str(out))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("# iso =", (out / "game.ini").read_text(encoding="utf-8"))
        self.assertEqual(json.loads((out / "port.json").read_text(encoding="utf-8"))["serial"], "SLES-50123")

    def test_invalid_inputs_are_rejected(self):
        cases = {
            "no pvd": ({"SYSTEM.CNF;1": b"BOOT2 = cdrom0:\\A.ELF;1\n", "A.ELF;1": mips_elf()}, {"pvd": False}, "ISO9660"),
            "no system.cnf": ({"A.ELF;1": mips_elf()}, {}, "SYSTEM.CNF"),
            "ps1 disc": ({"SYSTEM.CNF;1": b"BOOT = cdrom:\\SLUS_000.01;1\n"}, {}, "BOOT2"),
            "missing elf": ({"SYSTEM.CNF;1": b"BOOT2 = cdrom0:\\GONE.ELF;1\n"}, {}, "not on the disc"),
            "traversal": ({"SYSTEM.CNF;1": b"BOOT2 = cdrom0:\\..\\X.ELF;1\n"}, {}, "usable path"),
            "not mips": ({"SYSTEM.CNF;1": b"BOOT2 = cdrom0:\\A.ELF;1\n", "A.ELF;1": mips_elf(machine=62)}, {}, "not MIPS"),
            "64-bit": ({"SYSTEM.CNF;1": b"BOOT2 = cdrom0:\\A.ELF;1\n", "A.ELF;1": mips_elf(cls=2)}, {}, "32-bit"),
            "not elf": ({"SYSTEM.CNF;1": b"BOOT2 = cdrom0:\\A.ELF;1\n", "A.ELF;1": b"MZ" + b"\0" * 80}, {}, "not an ELF"),
        }
        for name, (files, kwargs, message) in cases.items():
            with self.subTest(name):
                iso = self.write_iso(files, **kwargs)
                out = self.dir / ("out-" + name.replace(" ", "-"))
                result = self.run_tool("--iso", str(iso), "--out", str(out))
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn(message, result.stderr)
                self.assertFalse((out / "game").exists(), "nothing is written for a rejected input")

    def test_output_inside_repository_or_non_empty_is_refused(self):
        source = self.dir / "SLUS_123.45"
        source.write_bytes(mips_elf())
        result = self.run_tool("--elf", str(source), "--out", str(ROOT / "build" / "new-port-test"))
        self.assertEqual(result.returncode, 3)
        self.assertIn("inside this repository", result.stderr)
        self.assertFalse((ROOT / "build" / "new-port-test").exists())
        out = self.dir / "used"
        out.mkdir()
        (out / "keep.txt").write_text("user file", encoding="utf-8")
        result = self.run_tool("--elf", str(source), "--out", str(out))
        self.assertEqual(result.returncode, 3)
        self.assertEqual(sorted(p.name for p in out.iterdir()), ["keep.txt"])
        result = self.run_tool("--elf", str(source), "--out", str(out), "--force")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((out / "keep.txt").read_text(encoding="utf-8"), "user file")


if __name__ == "__main__":
    unittest.main()
