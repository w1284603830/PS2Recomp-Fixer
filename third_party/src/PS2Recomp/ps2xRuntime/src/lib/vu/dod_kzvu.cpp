// [DoD] PCSX2's VU1 (microVU recompiler) as the VU1 of this port, through kzvu (KillzoneRecomp's ext/kzvu, GPL-3.0+,
// built from PCSX2 646df006a4 into build/kzvu/Release/kzvu.lib). It replaces PS2Recomp's VU1Interpreter, which ran
// MSCAL programs with a 65536-cycle budget and took most of the game thread in KillzoneRecomp's measurements; kzvu's
// own test compares the two (0 differences in 1776 checks here).
//
// Wiring (no header that generated code includes is touched): VU1 code/data memory become kzvu's buffers, MSCAL/MSCNT
// from VIF1 run kzvu to the program's E bit, XGKICK packets go to the GIF as PATH1. All of it on the game thread.
// Settings follow PCSX2's defaults for Shin Onimusha (its GameIndex entry has no VU options): clamp mode 1 (normal),
// IbitHack off, mVU flag hack on.
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "kzvu.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace dodkzvu
{
    namespace
    {
        PS2Runtime *g_runtime = nullptr;
        uint64_t g_codeGeneration = ~0ull;
        constexpr uint32_t kBudget = 1u << 24; // run programs to their E bit (VIF1 is synchronous here)

        void onXgkick(void *, const uint8_t *packet, uint32_t bytes, uint32_t)
        {
            g_runtime->memory().submitGifPacket(GifPathId::Path1, packet, bytes);
        }

        R5900Context *eeContext()
        {
            return g_runtime->eeScheduler().currentContext();
        }

        void prepare(uint32_t top, uint32_t itop)
        {
            const uint64_t generation = g_runtime->memory().getVU1CodeGeneration();
            if (generation != g_codeGeneration)
            {
                kzvuMicroWritten(0, kKzvuCodeSize);
                g_codeGeneration = generation;
            }
            const R5900Context *ctx = eeContext();
            kzvuSetFBRST(ctx ? ctx->vu0_fbrst : 0u);
            kzvuSetTop(top, itop);
        }

        void finish()
        {
            for (int i = 0; i < 16 && kzvuRunning() && (kzvuVpuStat() & 0x0600u) == 0u; ++i)
                kzvuContinue(kBudget);
            if (R5900Context *ctx = eeContext())
                ctx->vu0_vpu_stat = (ctx->vu0_vpu_stat & ~0x0600u) | (kzvuVpuStat() & 0x0600u);
        }
    }
}

// Called by the runner after PS2Runtime::initialize() and before the game starts. mode: "microvu" (default),
// "interp" (PCSX2's VU interpreter), "builtin" (keep PS2Recomp's VU1Interpreter).
bool dodKzvuInstall(PS2Runtime &runtime, const char *mode)
{
    using namespace dodkzvu;
    const std::string m = mode ? mode : "microvu";
    if (m == "builtin")
    {
        std::fprintf(stderr, "[vu1] PS2Recomp VU1 interpreter (vu1=builtin)\n");
        return false;
    }
    KzvuConfig config;
    config.useJit = m != "interp";
    config.clampMode = 1;
    config.iBitHack = false;
    config.flagHack = true;
    config.xgkick = &onXgkick;
    std::string error;
    if (!kzvuInit(config, &error))
    {
        std::fprintf(stderr, "[vu1] kzvu failed to start (%s); keeping the PS2Recomp interpreter\n", error.c_str());
        return false;
    }
    g_runtime = &runtime;
    PS2Memory &memory = runtime.memory();
    // VU1 memory becomes kzvu's (it holds whatever was there; nothing has run yet). The runtime's own buffers are not
    // freed: the process ends with _Exit.
    std::memcpy(kzvuCodeMem(), memory.getVU1Code(), kKzvuCodeSize);
    std::memcpy(kzvuDataMem(), memory.getVU1Data(), kKzvuDataSize);
    memory.m_vu1Code = kzvuCodeMem();
    memory.m_vu1Data = kzvuDataMem();
    memory.setVu1MscalCallback([](uint32_t startPC, uint32_t top, uint32_t itop)
                               {
                                   prepare(top, itop);
                                   kzvuExecute(startPC, kBudget);
                                   finish(); });
    memory.setVu1MscntCallback([](uint32_t top, uint32_t itop)
                               {
                                   prepare(top, itop);
                                   kzvuExecute(0xFFFFFFFFu, kBudget);
                                   finish(); });
    std::fprintf(stderr, "[vu1] %s (kzvu, PCSX2 646df006a4), clamp 1\n",
                 config.useJit ? "microVU recompiler" : "PCSX2 VU interpreter");
    return true;
}
