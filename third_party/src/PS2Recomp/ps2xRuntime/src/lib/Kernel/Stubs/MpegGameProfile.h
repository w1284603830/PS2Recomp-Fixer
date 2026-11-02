#pragma once

#include "GuestAddress.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ps2_stubs
{
    // These are supported library layouts, not automatic game detection.
    enum class MpegAbi { Unsupported, Work19A0, Legacy118 };
    enum class MpegEntry : size_t
    {
        Create, Delete, AddBs, GetPicture, Reset, GetDecodeMode, AddCallback,
        ClearRefBuff, Flush, GetPictureRAW8, GetPictureRAW8xy, Count
    };

    struct MpegInstructionGuard
    {
        uint32_t address = 0u;
        uint32_t instruction = 0u;
    };

    struct MpegGameProfile
    {
        MpegAbi abi = MpegAbi::Unsupported;
        // At least one executable/revision guard is required. Unused entries have address zero.
        std::array<MpegInstructionGuard, 4> guards{};
        std::array<uint32_t, 2> defaultCallbacks{};
        // Legacy118's globals are game data, not part of its object layout.
        uint32_t objectGlobal = 0u;
        uint32_t referenceTable = 0u;
        std::array<uint32_t, 9> references{};
        uint32_t resetGlobal = 0u;
        // Optional HD routing. Leave all these fields zero to use original video.
        int32_t movieIdGpOffset = 0;
        uint32_t movieFlags = 0u;
        uint32_t movieFlagsStride = 0u;
        uint32_t movieCount = 0u;
        std::array<uint32_t, static_cast<size_t>(MpegEntry::Count)> hdEntries{};
        // Optional revision-specific CSR.FIELD workaround; default off.
        uint32_t movieFieldReadPc = 0u;
        uint32_t movieFieldReadInstruction = 0u;
    };

    inline bool validMpegGameProfile(const MpegGameProfile &p)
    {
        if (p.abi != MpegAbi::Work19A0 && p.abi != MpegAbi::Legacy118)
            return false;
        bool guarded = false;
        for (const auto &g : p.guards)
        {
            if (!g.address)
                continue;
            guarded = true;
            if (!validProfileRamRange(g.address, 4u, 4u))
                return false;
        }
        if (!guarded)
            return false;
        if (p.movieFieldReadPc && !validProfileRamRange(p.movieFieldReadPc, 4u, 4u))
            return false;
        for (uint32_t callback : p.defaultCallbacks)
            if (!callback || !validProfileRamRange(callback, 4u, 4u))
                return false;
        if (p.abi == MpegAbi::Legacy118)
        {
            if (!p.objectGlobal || !validProfileRamRange(p.objectGlobal, 4u, 4u) ||
                !p.referenceTable || !validProfileRamRange(p.referenceTable, 36u, 4u) ||
                !p.resetGlobal || !validProfileRamRange(p.resetGlobal, 4u, 4u))
                return false;
            for (uint32_t reference : p.references)
                if (!reference || !validProfileRamRange(reference, 0x68u, 4u))
                    return false;
        }
        bool hooks = false;
        for (uint32_t entry : p.hdEntries)
            hooks |= entry != 0u;
        if (hooks)
        {
            if (p.abi != MpegAbi::Work19A0 || !p.movieCount)
                return false;
            for (uint32_t entry : p.hdEntries)
                if (!entry || !validProfileRamRange(entry, 4u, 4u))
                    return false;
            for (size_t i = 0; i < p.hdEntries.size(); ++i)
                for (size_t j = i + 1; j < p.hdEntries.size(); ++j)
                    if (p.hdEntries[i] == p.hdEntries[j])
                        return false;
        }
        if (p.movieCount)
        {
            const uint64_t bytes = static_cast<uint64_t>(p.movieCount - 1u) * p.movieFlagsStride + 2u;
            if (p.abi != MpegAbi::Work19A0 || !p.movieFlags || p.movieFlagsStride < 2u ||
                (p.movieFlagsStride & 1u) || !validProfileRamRange(p.movieFlags, bytes, 2u))
                return false;
        }
        return true;
    }
}
