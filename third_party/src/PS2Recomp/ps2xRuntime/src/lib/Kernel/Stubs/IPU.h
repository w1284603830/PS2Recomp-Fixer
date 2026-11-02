#pragma once

#include "ps2_stubs.h"

namespace ps2_stubs
{
    // Required only for the HLE sceIpuInit path. IQ points to 80 aligned bytes, VQ to 32 aligned bytes in EE RAM.
    // helper=0 (or an unregistered helper) writes D4_CHCR directly; there are no per-game default addresses.
    void dodConfigureIpuInit(uint32_t iqBase, uint32_t vqBase, uint32_t setD4ChcrEntry = 0u);
    bool hasValidIpuInitConfig();
    void resetIpuInitConfig();
    void sceIpuInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceIpuRestartDMA(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceIpuStopDMA(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceIpuSync(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
}
