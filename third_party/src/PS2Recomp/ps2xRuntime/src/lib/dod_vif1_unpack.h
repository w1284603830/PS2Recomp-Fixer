#pragma once
// [DoD] VIF1 UNPACK specialised per format (RUNTIME_PATCHES row 121). Same semantics as the per-element version in
// ps2_vif1_interpreter.cpp (PCSX2 Vif_Unpack.cpp via KillzoneRecomp patch 0010), but VN/VL, the mask bit and STMOD
// are template parameters, so the per-element format switch, the mask lookups and the mode switch compile away.
// vif1UnpackPcsx2 was 6.4% of the EE thread in room 0x386. Checked against the per-element version by
// tools/scripts/diag/vif1_unpack_diff (random commands, registers and data, byte-exact VU memory and registers).
// Regs is any struct with cycle, mode, num, mask, tops, row[4], col[4] (VIFRegisters, or the test's copy).
#include <array>
#include <cstdint>
#include <cstring>
#include <utility>

namespace dod_vif1_unpack
{
    // Bytes of source data per vector, by VN<<2|VL (PCSX2 nVifT).
    constexpr uint8_t kGsize[16] = {4, 2, 1, 0, 8, 4, 2, 0, 12, 6, 3, 0, 16, 8, 4, 2};

    template <class Regs, uint32_t VNVL, bool MASK, uint32_t MODE>
    void unpack(uint8_t *vuMem, uint32_t vuMemSize, Regs &regs, uint32_t cmd, const uint8_t *data, uint32_t dataBytes)
    {
        constexpr uint32_t vsize = kGsize[VNVL];
        if constexpr (vsize == 0u)
        {
            (void)vuMem, (void)vuMemSize, (void)regs, (void)cmd, (void)data, (void)dataBytes;
            return; // invalid unpack (PCSX2 only warns)
        }
        else
        {
            constexpr uint32_t vn = VNVL >> 2;
            const bool usn = (cmd & 0x4000u) != 0u;
            uint32_t num = (cmd >> 16) & 0xFFu;
            if (num == 0u)
                num = 256u;
            const uint32_t cl = regs.cycle & 0xFFu;
            const uint32_t wl = ((regs.cycle >> 8) & 0xFFu) ? ((regs.cycle >> 8) & 0xFFu) : 256u;
            const bool isFill = cl < wl;
            uint32_t addr = cmd & 0x3FFu;
            if ((cmd & 0x8000u) != 0u)
                addr += regs.tops;
            addr = (addr << 4) & 0x3FF0u;
            const uint32_t skipBytes = isFill ? 0u : (cl - wl) * 16u;
            const uint32_t memMask = vuMemSize - 16u;

            auto element = [&](uint32_t offset, uint32_t index) -> uint32_t
            {
                const uint32_t at = offset + index * (4u >> (VNVL & 3u));
                if (at + (4u >> (VNVL & 3u)) > dataBytes)
                    return 0u;
                if constexpr ((VNVL & 3u) == 0u)
                {
                    uint32_t v;
                    std::memcpy(&v, data + at, 4);
                    return v;
                }
                else if constexpr ((VNVL & 3u) == 1u)
                {
                    uint16_t v;
                    std::memcpy(&v, data + at, 2);
                    return usn ? static_cast<uint32_t>(v) : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(v)));
                }
                else
                {
                    const uint8_t v = data[at];
                    return usn ? static_cast<uint32_t>(v) : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(v)));
                }
            };

            auto write = [&](uint32_t *dest, uint32_t f, uint32_t value)
            {
                if constexpr (MODE == 1u)
                    dest[f] = value + regs.row[f];
                else if constexpr (MODE == 2u)
                {
                    regs.row[f] += value;
                    dest[f] = regs.row[f];
                }
                else if constexpr (MODE == 3u)
                {
                    regs.row[f] = value;
                    dest[f] = value;
                }
                else
                    dest[f] = value;
            };

            uint32_t src = 0u; // byte offset of the current source vector
            uint32_t vcl = 0u; // write-cycle counter (PCSX2 vif.cl)
            for (uint32_t n = 0; n < num; ++n)
            {
                uint32_t in[4];
                if constexpr (VNVL == 15u)
                {
                    uint16_t v = 0;
                    if (src + 2u <= dataBytes)
                        std::memcpy(&v, data + src, 2);
                    in[0] = (v & 0x001Fu) << 3;
                    in[1] = (v & 0x03E0u) >> 2;
                    in[2] = (v & 0x7C00u) >> 7;
                    in[3] = (v & 0x8000u) >> 8;
                }
                else if constexpr (vn == 0u)
                {
                    in[0] = in[1] = in[2] = in[3] = element(src, 0);
                }
                else if constexpr (vn == 1u)
                {
                    in[0] = in[2] = element(src, 0);
                    in[1] = in[3] = element(src, 1);
                }
                else // V3 and V4 read four elements
                {
                    for (uint32_t c = 0; c < 4u; ++c)
                        in[c] = element(src, c);
                }

                uint32_t *dest = reinterpret_cast<uint32_t *>(vuMem + (addr & memMask));
                if constexpr (!MASK)
                {
                    for (uint32_t f = 0; f < 4u; ++f)
                        write(dest, f, in[f]);
                }
                else
                {
                    const uint32_t maskCycle = vcl < 3u ? vcl : 3u;
                    for (uint32_t f = 0; f < 4u; ++f)
                    {
                        switch ((regs.mask >> (maskCycle * 8u + f * 2u)) & 3u)
                        {
                        case 0: write(dest, f, in[f]); break;
                        case 1: dest[f] = regs.row[f]; break;
                        case 2: dest[f] = regs.col[maskCycle]; break;
                        default: break; // write protect
                        }
                    }
                }

                addr += 16u;
                ++vcl;
                if (isFill)
                {
                    if (vcl <= cl)
                        src += vsize;
                    else if (vcl == wl)
                        vcl = 0u;
                }
                else
                {
                    src += vsize;
                    if (vcl >= wl)
                    {
                        addr += skipBytes;
                        vcl = 0u;
                    }
                }
            }
            regs.num = 0u;
        }
    }

    template <class Regs>
    using UnpackFn = void (*)(uint8_t *, uint32_t, Regs &, uint32_t, const uint8_t *, uint32_t);

    // Index: VN<<5 | VL<<3 | mask bit<<2 | STMOD.
    template <class Regs, size_t... K>
    constexpr std::array<UnpackFn<Regs>, sizeof...(K)> makeTable(std::index_sequence<K...>)
    {
        return {{&unpack<Regs, static_cast<uint32_t>(K >> 3), ((K >> 2) & 1u) != 0u, static_cast<uint32_t>(K & 3u)>...}};
    }

    template <class Regs>
    void unpackDispatch(uint8_t *vuMem, uint32_t vuMemSize, Regs &regs, uint32_t cmd, const uint8_t *data,
                        uint32_t dataBytes)
    {
        static constexpr auto table = makeTable<Regs>(std::make_index_sequence<128>{});
        const uint32_t upk = (cmd >> 24) & 0x1Fu;
        const uint32_t vnvl = upk & 0xFu;
        const uint32_t mode = (vnvl == 15u) ? 0u : (regs.mode & 3u); // V4-5 ignores STMOD
        table[(vnvl << 3) | (((upk >> 4) & 1u) << 2) | mode](vuMem, vuMemSize, regs, cmd, data, dataBytes);
    }
}
