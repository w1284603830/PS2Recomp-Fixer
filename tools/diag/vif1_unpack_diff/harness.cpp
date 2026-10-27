// [DoD] Offline differential test for the specialised VIF1 UNPACK (ps2xRuntime/src/lib/dod_vif1_unpack.h,
// RUNTIME_PATCHES row 121) against the per-element version it replaces (copied verbatim below from
// ps2_vif1_interpreter.cpp vif1UnpackPcsx2, build 286). Random UNPACK codes (every VN/VL, mask bit, USN, FLG, address,
// NUM), registers (CL/WL including fill mode and WL=0, STMOD, MASK, TOPS, ROW, COL) and source data (including too
// short streams); both versions run on copies of the same 16 KB VU memory and registers, which must end up equal.
// Build: build.cmd. Run: work\tests\vif1_unpack_diff\vif1_unpack_diff.exe [trials]
#include "dod_vif1_unpack.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

struct Regs
{
    uint32_t cycle = 0, mode = 0, num = 0, mask = 0, tops = 0;
    uint32_t row[4] = {}, col[4] = {};
};

namespace reference
{
    constexpr uint8_t kVifGsize[16] = {4, 2, 1, 0, 8, 4, 2, 0, 12, 6, 3, 0, 16, 8, 4, 2};

    void vif1UnpackPcsx2(uint8_t *vuMem, uint32_t vuMemSize, Regs &regs, uint32_t cmd, const uint8_t *data,
                         uint32_t dataBytes)
    {
        const uint32_t upk = (cmd >> 24) & 0x1Fu; // mask bit | VN | VL
        const uint32_t vnvl = upk & 0xFu;
        const bool doMask = (upk & 0x10u) != 0u;
        const bool usn = (cmd & 0x4000u) != 0u;
        const uint32_t vsize = kVifGsize[vnvl];
        if (vsize == 0u)
            return; // invalid unpack (PCSX2 only warns)
        uint32_t num = (cmd >> 16) & 0xFFu;
        if (num == 0u)
            num = 256u;
        const uint32_t cl = regs.cycle & 0xFFu;
        const uint32_t wl = ((regs.cycle >> 8) & 0xFFu) ? ((regs.cycle >> 8) & 0xFFu) : 256u;
        const bool isFill = cl < wl;
        const uint32_t mode = (vnvl == 15u) ? 0u : (regs.mode & 3u);
        uint32_t addr = cmd & 0x3FFu;
        if ((cmd & 0x8000u) != 0u)
            addr += regs.tops;
        addr = (addr << 4) & 0x3FF0u;
        const uint32_t skipBytes = isFill ? 0u : (cl - wl) * 16u;
        const uint32_t vl = vnvl & 3u;
        const uint32_t vn = vnvl >> 2;

        auto element = [&](uint32_t offset, uint32_t index) -> uint32_t
        {
            const uint32_t bytes = 4u >> vl; // 4, 2, 1
            const uint32_t at = offset + index * bytes;
            if (at + bytes > dataBytes)
                return 0u;
            if (vl == 0u)
            {
                uint32_t v;
                std::memcpy(&v, data + at, 4);
                return v;
            }
            if (vl == 1u)
            {
                uint16_t v;
                std::memcpy(&v, data + at, 2);
                return usn ? static_cast<uint32_t>(v) : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(v)));
            }
            const uint8_t v = data[at];
            return usn ? static_cast<uint32_t>(v) : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(v)));
        };

        uint32_t src = 0u; // byte offset of the current source vector
        uint32_t vcl = 0u; // write-cycle counter (PCSX2 vif.cl)
        for (uint32_t n = 0; n < num; ++n)
        {
            uint32_t in[4];
            if (vnvl == 15u)
            {
                uint16_t v = 0;
                if (src + 2u <= dataBytes)
                    std::memcpy(&v, data + src, 2);
                in[0] = (v & 0x001Fu) << 3;
                in[1] = (v & 0x03E0u) >> 2;
                in[2] = (v & 0x7C00u) >> 7;
                in[3] = (v & 0x8000u) >> 8;
            }
            else if (vn == 0u)
            {
                in[0] = in[1] = in[2] = in[3] = element(src, 0);
            }
            else if (vn == 1u)
            {
                in[0] = in[2] = element(src, 0);
                in[1] = in[3] = element(src, 1);
            }
            else // V3 and V4 read four elements
            {
                for (uint32_t c = 0; c < 4u; ++c)
                    in[c] = element(src, c);
            }

            uint32_t *dest = reinterpret_cast<uint32_t *>(vuMem + (addr & (vuMemSize - 16u)));
            const uint32_t maskCycle = vcl < 3u ? vcl : 3u;
            for (uint32_t f = 0; f < 4u; ++f)
            {
                const uint32_t m = doMask ? (regs.mask >> (maskCycle * 8u + f * 2u)) & 3u : 0u;
                switch (m)
                {
                case 0:
                    switch (mode)
                    {
                    case 1: dest[f] = in[f] + regs.row[f]; break;
                    case 2: regs.row[f] += in[f]; dest[f] = regs.row[f]; break;
                    case 3: regs.row[f] = in[f]; dest[f] = in[f]; break;
                    default: dest[f] = in[f]; break;
                    }
                    break;
                case 1: dest[f] = regs.row[f]; break;
                case 2: dest[f] = regs.col[maskCycle]; break;
                default: break; // write protect
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

static uint64_t g_rng = 0x2545F4914F6CDD1Dull;
static uint32_t rnd()
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return static_cast<uint32_t>(g_rng >> 16);
}

int main(int argc, char **argv)
{
    const long trials = argc > 1 ? std::atol(argv[1]) : 2000000L;
    constexpr uint32_t kVuSize = 0x4000u;
    std::vector<uint8_t> memA(kVuSize), memB(kVuSize), data(4096 + 64);
    long perFormat[32] = {};
    for (long t = 0; t < trials; ++t)
    {
        const uint32_t upk = rnd() & 0x1Fu;
        uint32_t numField = (rnd() & 3u) == 0u ? (rnd() & 0xFFu) : (rnd() % 33u);
        uint32_t cmd = (upk << 24) | (numField << 16) | (rnd() & 0x3FFu) | (rnd() & 0x4000u) | (rnd() & 0x8000u);
        Regs regs;
        const uint32_t pick = rnd() % 8u;
        uint32_t cl = rnd() % 9u, wl = rnd() % 9u;
        if (pick == 0u)
            cl = rnd() & 0xFFu, wl = rnd() & 0xFFu; // anything, WL 0 = 256
        else if (pick < 4u)
            wl = cl; // the common straight copy
        regs.cycle = (wl << 8) | cl | (rnd() & 0xFFFF0000u);
        regs.mode = rnd();
        regs.mask = rnd() | (rnd() << 16);
        regs.tops = rnd() & 0x3FFu;
        for (int i = 0; i < 4; ++i)
            regs.row[i] = rnd() | (rnd() << 16), regs.col[i] = rnd() | (rnd() << 16);
        regs.num = rnd();
        const uint32_t vnvl = upk & 0xFu;
        const uint32_t num = numField ? numField : 256u;
        const uint32_t need = num * dod_vif1_unpack::kGsize[vnvl];
        uint32_t dataBytes = (rnd() & 3u) ? need : (rnd() % (need + 40u));
        if (dataBytes > 4096u)
            dataBytes = 4096u;
        for (auto &b : data)
            b = static_cast<uint8_t>(rnd());
        for (uint32_t i = 0; i < kVuSize; ++i)
            memA[i] = static_cast<uint8_t>(rnd());
        memB = memA;
        Regs ra = regs, rb = regs;
        reference::vif1UnpackPcsx2(memA.data(), kVuSize, ra, cmd, data.data(), dataBytes);
        dod_vif1_unpack::unpackDispatch(memB.data(), kVuSize, rb, cmd, data.data(), dataBytes);
        if (memA != memB || std::memcmp(&ra, &rb, sizeof(Regs)) != 0)
        {
            uint32_t at = 0;
            while (at < kVuSize && memA[at] == memB[at])
                ++at;
            std::printf("MISMATCH trial %ld cmd %08X cycle %08X mode %u mask %08X dataBytes %u first byte %u\n", t, cmd,
                        regs.cycle, regs.mode & 3u, regs.mask, dataBytes, at);
            return 1;
        }
        ++perFormat[upk];
    }
    std::printf("all equal: %ld trials;", trials);
    for (int i = 0; i < 32; ++i)
        std::printf(" %02X:%ld", i, perFormat[i]);
    std::printf("\n");
    return 0;
}
