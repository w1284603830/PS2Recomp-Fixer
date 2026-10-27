#pragma once

// [DoD] PS2 IPU (MPEG-2 decoder) for the runtime: registers 0x10002000-0x10002030, the input FIFO fed by
// DMA channel 4 (toIPU) or 0x10007010, and the output FIFO drained by DMA channel 3 (fromIPU) or 0x10007000.
// The decoding follows PCSX2's IPU (pcsx2/IPU/IPU_MultiISA.cpp, GPL-2.0+), which is based on libmpeg2.
//
// Model: commands run as soon as they are written, as far as the input already received allows; a command that
// runs out of input stays busy and resumes when more input arrives. Output is queued without the 8-qword
// FIFO limit, so a DMA from the IPU can take as much as has been decoded.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ps2_ipu
{
    void reset();

    // Register offsets relative to 0x10002000: 0x00/0x04 IPU_CMD (data, busy), 0x10 IPU_CTRL, 0x20 IPU_BP,
    // 0x30/0x34 IPU_TOP (data, busy).
    uint32_t read32(uint32_t offset);
    void write32(uint32_t offset, uint32_t value);

    void pushInput(const uint8_t *data, size_t bytes);
    size_t outputBytes();
    size_t popOutput(uint8_t *dst, size_t bytes);

    // Diagnostics for --trace.
    struct Stats
    {
        uint64_t commands = 0;
        uint64_t macroblocks = 0;
        uint64_t inputBytes = 0;
        uint64_t outputBytes = 0;
        uint64_t errors = 0;
    };
    Stats stats();
}
