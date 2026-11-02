#pragma once

#include "ps2_stubs.h"
#include "GuestAddress.h"
#include <array>
#include <cstddef>

namespace ps2_stubs
{
    // The implemented SCEE font ABI has 0x24-byte entries. Its globals belong to the port.
    enum class FontGlobal : size_t { Clut, Width, Height, Mode, Colour, ScaleY, ScaleX, FontId, Count };
    struct FontGameProfile
    {
        uint32_t table = 0u;
        uint32_t count = 0u;
        uint32_t signatureAddress = 0u;
        uint32_t signatureWord = 0u;
        std::array<int32_t, static_cast<size_t>(FontGlobal::Count)> gpOffsets{};
    };
    bool configureFontGameProfile(const FontGameProfile &profile);
    void resetFontGameProfile();
    void sceeFontInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceeFontLoadFont(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceeFontGenerateString(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceeFontPrintfAt(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceeFontPrintfAt2(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceeFontClose(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceeFontSetColour(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceeFontSetMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceeFontSetFont(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceeFontSetScale(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
}
