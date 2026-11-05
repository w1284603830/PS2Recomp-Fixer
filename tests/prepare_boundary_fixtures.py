"""Extract production HLE bodies for the standalone boundary test target."""
from pathlib import Path
import sys
from test_particle_edges import function
ROOT=Path(__file__).resolve().parents[1]
LIB=ROOT/'third_party/src/PS2Recomp/ps2xRuntime/src/lib'

def prepare(directory):
    directory=Path(directory);directory.mkdir(parents=True,exist_ok=True)
    support=(LIB/'Kernel/Stubs/Helpers/Support.h').read_text()
    vu=(LIB/'Kernel/Stubs/VU.cpp').read_text()
    sif=(LIB/'Kernel/Stubs/SIF.cpp').read_text()
    mc=(LIB/'Kernel/Stubs/MemoryCard.cpp').read_text()
    structs=support[support.index('    struct GsDispEnvMem'):support.index('    constexpr uint32_t kGsParamScratchOffset')]
    text=structs
    for name in ('readGsImage','writeGsImage','writeGsDispEnv','readGsDispEnv','readGsDBuffDc','readGsDBuff','writeGsDBuffDc','writeGsDBuff','readGsRegPairs'):
        text+=function(support,'    static bool '+name+'(')
    for name in ('readVuVec4f','readVuVec4i','readVuMatrix4f','writeVuVec4f','writeVuVec4i','writeVuMatrix4f'):
        text+=function(vu,'        bool '+name+'(')
    for name in ('sceVu0CopyMatrix','sceVu0CopyVector','sceVu0CopyVectorXYZ'):
        text+=function(vu,'    void '+name+'(')
    text+=function(mc,'        void writeMcCString(')
    for name in ('isCopyableGuestAddress','canAccessEeRange','readEeRange','writeEeRange'):
        text+=function(sif,'        bool '+name+'(')
    for name in ('sceSifSetDma','sceSifGetOtherData'):
        text+=function(sif,'    void '+name+'(')
    (directory/'memory_hle_current.inc').write_text(text)

if __name__=='__main__':prepare(sys.argv[1])
