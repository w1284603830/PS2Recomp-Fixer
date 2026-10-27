#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dodpcsx2 {
uint64_t queuedVsyncs();
void cgSubmit(uint32_t frame, bool eligible);
}

// [DoD] The story-CG uploader builds two DMA chains per decoded RGB frame.
// For interleaved rows (area flag 0x40 clear), chain 0 starts one source row
// later than chain 1. Keep both at the even source rows on a progressive
// display. The other branch uses separately stored fields and is unchanged.
// DOD_CG_FIELD=keep restores the original row selection.
inline bool dodCgKeepOriginalRowOffset(const uint8_t* rdram)
{
    static const bool keep = [] {
        const char* value = std::getenv("DOD_CG_FIELD");
        return value && std::strcmp(value, "keep") == 0;
    }();
    uint32_t instruction = 0;
    std::memcpy(&instruction, rdram + 0x2adbdcu, sizeof(instruction));
    return keep || instruction != 0x00c33021u;
}

// Default-off, bounded CPU-side submission trace, without GPU readback or
// desktop capture. "frame" is the decoder counter, not a host FPS estimate.
inline void dodCgSubmit(const uint8_t* rdram, const R5900Context* ctx)
{
    const uint32_t gp = GPR_U32(ctx, 28), player = GPR_U32(ctx, 18);
    if (gp < 0x7e0cu || gp >= 0x2000000u || player > 0x1ffffe8u)
        return;
    uint32_t frame = 0, width = 0, height = 0, area = 0;
    std::memcpy(&frame, rdram + gp - 0x7660u, sizeof(frame));
    std::memcpy(&width, rdram + player + 0x10u, sizeof(width));
    std::memcpy(&height, rdram + player + 0x14u, sizeof(height));
    std::memcpy(&area, rdram + gp - 0x7e0cu, sizeof(area));
    uint16_t flags = 0x40u;
    if (area < 4096u)
        std::memcpy(&flags, rdram + 0x4df7d6u + area * 8u, sizeof(flags));
    const bool eligible = width == 512 && height == 336 && !(flags & 0x40u) &&
                          !dodCgKeepOriginalRowOffset(rdram);
    dodpcsx2::cgSubmit(frame, eligible);
    static const bool on = std::getenv("DOD_CG_TRACE") != nullptr;
    if (!on)
        return;
    static unsigned count = 0;
    uint32_t field = 0;
    if (frame < 120 || count >= 600)
        return;
    std::memcpy(&field, rdram + 0x817734u, sizeof(field));
    const auto now = std::chrono::steady_clock::now();
    static const auto start = now;
    const double ms = std::chrono::duration<double, std::milli>(now - start).count();
    ++count;
    std::fprintf(stderr, "[cg-submit] n %u field %u frame %u buffer %u chain %08X ms %.3f queued %llu\n",
                 count, field, frame, unsigned(rdram[0x81772au]),
                 GPR_U32(ctx, 19), ms, dodpcsx2::queuedVsyncs());
}
