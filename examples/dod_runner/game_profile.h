#pragma once

#include "../lib/Kernel/Stubs/MPEG.h"

// Copy beside this reference runner when installing it in ps2xRuntime/src/runner/.
// SLPM_662.75 only. These addresses are not shared-runtime defaults.
inline ps2_stubs::MpegGameProfile dodMpegGameProfile()
{
    ps2_stubs::MpegGameProfile p;
    p.abi = ps2_stubs::MpegAbi::Work19A0;
    p.guards = {{{0x10AC38u, 0x27BDFFB0u}, {0x10AC7Cu, 0x28C219A0u},
                 {0x10BA30u, 0x8C880040u}, {0x10BA34u, 0x8D0200A8u}}};
    p.defaultCallbacks = {0x10BEE8u, 0x10BF10u};
    p.movieIdGpOffset = -0x7E0C;
    p.movieFlags = 0x4DF7D6u;
    p.movieFlagsStride = 8u;
    p.movieCount = 109u;
    p.movieFieldReadPc = 0x3BB2F0u;
    p.movieFieldReadInstruction = 0x8C281000u;
    // MpegEntry order: Create, Delete, AddBs, GetPicture, Reset, GetDecodeMode,
    // AddCallback, ClearRefBuff, Flush, RAW8, RAW8xy.
    p.hdEntries = {0x10AC38u, 0x10B890u, 0x10B898u, 0x10B8D8u, 0x10B978u, 0x10BA30u,
                   0x10BA50u, 0x10BC08u, 0x10C8A8u, 0x10B920u, 0x10BC60u};
    return p;
}
