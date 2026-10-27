// Based on Blackline Interactive implementation
#include "runtime/ps2_memory.h"
#include "gs/dod_pcsx2_gs.h"
#include "dod_vif1_unpack.h"
#include <cstring>
#include <cstdio>
#include <string>
#include <cstdlib>

uint64_t g_dodVif1OpcodeCounts[128] = {}; // [DoD] VIF1 commands seen, by opcode (diagnostics for --trace)
static uint32_t g_dodDirectLeftQw = 0;     // [DoD] DIRECT payload still to come from the next VIF1 buffer
static bool g_dodDirectLeftHl = false;

// [DoD] VIF1_FBRST.RST (ps2_memory.cpp): the command in progress is dropped.
void dodVif1ResetCommand()
{
    g_dodDirectLeftQw = 0u;
    g_dodDirectLeftHl = false;
}
void dodVif1Ensure(const uint8_t *data, uint32_t endOffset); // [DoD] ps2_memory.cpp: lazy DMA chain data
uint32_t dodVif1SourceOf(const uint8_t *data, uint32_t offset); // [DoD] ps2_memory.cpp: diagnostics
void dodVif1ChainNotesAround(uint32_t offset);                    // [DoD] ps2_memory.cpp: diagnostics

// [DoD] DOD_VIF_LOG=<n>: the first n VIF1 commands whose data runs past the end of the DMA transfer that brings them,
// and unknown commands (both drop the rest of the transfer).
// [DoD] DOD_VIF_EVENTS=<n>: the first n VIF1 register events (STCYCL, FBRST, CYCLE writes) on stderr.
void dodVif1Event(const char *what, uint32_t value, uint32_t extra)
{
    static const int limit = []() { const char *v = std::getenv("DOD_VIF_EVENTS"); return v ? std::atoi(v) : 0; }();
    static int logged = 0;
    if (logged >= limit)
        return;
    ++logged;
    std::fprintf(stderr, "[vif1-event] %s %08X (%u)\n", what, value, extra);
}

struct DodVif1Recent
{
    uint32_t pos, cmd, cycle;
    const uint8_t *data;
};
static DodVif1Recent g_dodVif1Recent[16] = {};
static uint32_t g_dodVif1RecentAt = 0;

static void dodVif1Note(const char *what, uint32_t cmd, uint32_t pos, uint32_t need, uint32_t sizeBytes)
{
    static const int limit = []() { const char *v = std::getenv("DOD_VIF_LOG"); return v ? std::atoi(v) : 0; }();
    static int logged = 0;
    if (logged >= limit)
        return;
    ++logged;
    std::fprintf(stderr, "[vif1] %s code %08X at +%u needs %u bytes, transfer %u bytes\n", what, cmd, pos - 4u, need, sizeBytes);
    std::string recent;
    for (uint32_t k = 0; k < 16u; ++k)
    {
        const DodVif1Recent &r = g_dodVif1Recent[(g_dodVif1RecentAt + k) & 15u];
        char item[64];
        std::snprintf(item, sizeof(item), " +%u:%08X(cyc %04X @%08X)", r.pos, r.cmd, r.cycle & 0xFFFFu,
                      r.data ? dodVif1SourceOf(r.data, r.pos) : 0u);
        recent += item;
    }
    std::fprintf(stderr, "[vif1]   last commands:%s\n", recent.c_str());
    dodVif1ChainNotesAround(pos - 4u);
}

enum VIFCmd : uint8_t
{
    VIF_NOP = 0x00,
    VIF_STCYCL = 0x01,
    VIF_OFFSET = 0x02,
    VIF_BASE = 0x03,
    VIF_ITOP = 0x04,
    VIF_STMOD = 0x05,
    VIF_MSKPATH3 = 0x06,
    VIF_MARK = 0x07,
    VIF_FLUSHE = 0x10,
    VIF_FLUSH = 0x11,
    VIF_FLUSHA = 0x13,
    VIF_MSCAL = 0x14,
    VIF_MSCALF = 0x15,
    VIF_MSCNT = 0x17,
    VIF_STMASK = 0x20,
    VIF_STROW = 0x30,
    VIF_STCOL = 0x31,
    VIF_MPG = 0x4A,
    VIF_DIRECT = 0x50,
    VIF_DIRECTHL = 0x51,
};

namespace
{
    constexpr uint8_t kGifFmtImage = 2u;

    // [DoD] DOD_VIF_UNPACK=legacy keeps PS2Recomp's original VIF1 UNPACK code (A/B comparison).
    bool dodLegacyVif1Unpack()
    {
        static const bool legacy = []
        {
            const char *v = std::getenv("DOD_VIF_UNPACK");
            return v && std::strcmp(v, "legacy") == 0;
        }();
        return legacy;
    }

    // [DoD] DOD_VIF_UNPACK=generic keeps the per-element version below instead of dod_vif1_unpack.h (row 121).
    bool dodGenericVif1Unpack()
    {
        static const bool generic = []
        {
            const char *v = std::getenv("DOD_VIF_UNPACK");
            return v && std::strcmp(v, "generic") == 0;
        }();
        return generic;
    }

    // Bytes of source data per vector, by VN<<2|VL (PCSX2 nVifT).
    constexpr uint8_t kVifGsize[16] = {4, 2, 1, 0, 8, 4, 2, 0, 12, 6, 3, 0, 16, 8, 4, 2};

    // [DoD] VIF1 UNPACK with PCSX2's semantics (Vif_Unpack.cpp), from KillzoneRecomp patch 0010 (GPL-3.0):
    //  - WL = 0 means 256; fill mode is CL < WL. In fill mode the source pointer only advances for the first CL
    //    writes of a block; the other writes see the next (not yet consumed) vector, normally masked to row/col.
    //  - S-xx broadcasts; V2 writes x,y,x,y; V3 reads four elements (W = the next vector's first element);
    //    V4-5 expands 5:5:5:1 to 8-bit channels (<<3) and ignores STMOD.
    //  - STMOD 1 adds the row, 2 adds and stores the row, 3 stores the data into the row.
    //  - The mask row is chosen by the write-cycle counter (min(cl, 3)), the column register the same way.
    //  - The destination address wraps in VU1 memory (0x3FF0).
    // data/dataBytes is the source stream for this UNPACK; reads past it return zero.
    void vif1UnpackPcsx2(uint8_t *vuMem, uint32_t vuMemSize, VIFRegisters &regs, uint32_t cmd, const uint8_t *data,
                         uint32_t dataBytes)
    {
        if (!dodGenericVif1Unpack())
        {
            dod_vif1_unpack::unpackDispatch(vuMem, vuMemSize, regs, cmd, data, dataBytes);
            return;
        }
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

    uint32_t pendingGifImageQwc(const uint8_t *data, uint32_t sizeBytes)
    {
        if (!data || sizeBytes < 16u)
            return 0u;

        uint32_t offset = 0u;
        while (offset + 16u <= sizeBytes)
        {
            uint64_t tagLo = 0u;
            std::memcpy(&tagLo, data + offset, sizeof(tagLo));
            offset += 16u;

            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;

            uint64_t payloadBytes = 0u;
            if (flg == 0u) // PACKED
            {
                payloadBytes = static_cast<uint64_t>(nloop) * nreg * 16ull;
            }
            else if (flg == 1u) // REGLIST, padded to a quadword
            {
                payloadBytes = static_cast<uint64_t>(nloop) * nreg * 8ull;
                payloadBytes = (payloadBytes + 15ull) & ~15ull;
            }
            else // IMAGE; FLG 3 behaves as IMAGE [DoD]
            {
                payloadBytes = static_cast<uint64_t>(nloop) * 16ull;
                const uint64_t availableBytes = sizeBytes - offset;
                if (payloadBytes > availableBytes)
                {
                    return static_cast<uint32_t>((payloadBytes - availableBytes) / 16ull);
                }
            }

            if (payloadBytes > static_cast<uint64_t>(sizeBytes - offset))
                return 0u;
            offset += static_cast<uint32_t>(payloadBytes);
        }

        return 0u;
    }
}

void PS2Memory::processVIF0Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (sizeBytes == 0u || srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF0Data(m_rdram + srcPhys, sizeBytes);
}

void PS2Memory::processVIF0Data(const uint8_t *data, uint32_t sizeBytes)
{
    if (sizeBytes == 0u)
        return;

    uint32_t pos = 0;
    while (pos + 4 <= sizeBytes)
    {
        uint32_t cmd = 0u;
        std::memcpy(&cmd, data + pos, sizeof(cmd));
        pos += 4u;

        const uint8_t opcode = static_cast<uint8_t>((cmd >> 24) & 0x7Fu);
        const uint16_t imm = static_cast<uint16_t>(cmd & 0xFFFFu);
        const uint8_t num = static_cast<uint8_t>((cmd >> 16) & 0xFFu);
        const bool irq = (cmd & 0x80000000u) != 0u;

        vif0_regs.code = cmd;
        vif0_regs.num = num;
        if (irq)
            vif0_regs.stat |= (1u << 11);

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif0_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            vif0_regs.itops = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif0_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif0_regs.mark = imm;
            vif0_regs.stat |= (1u << 6);
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4u > sizeBytes)
                break;
            std::memcpy(&vif0_regs.mask, data + pos, sizeof(vif0_regs.mask));
            pos += 4u;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16u > sizeBytes)
                break;
            std::memcpy(vif0_regs.row, data + pos, 16u);
            pos += 16u;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16u > sizeBytes)
                break;
            std::memcpy(vif0_regs.col, data + pos, 16u);
            pos += 16u;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            const uint32_t destAddr = static_cast<uint32_t>(imm & 0x1FFu) * 8u;
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            uint32_t copyBytes = 0u;
            if (m_vu0Code && destAddr < PS2_VU0_CODE_SIZE && mpgBytes > 0u)
            {
                copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU0_CODE_SIZE)
                    copyBytes = PS2_VU0_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                {
                    std::memcpy(m_vu0Code + destAddr, data + pos, copyBytes);
                    markVU0CodeModified();
                }
            }

            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if ((opcode & 0x60u) == 0x60u)
        {
            const uint8_t vn = static_cast<uint8_t>((opcode >> 2) & 0x3u);
            const uint8_t vl = static_cast<uint8_t>(opcode & 0x3u);
            const int components = static_cast<int>(vn) + 1;
            int bitsPerComponent = 32;
            switch (vl)
            {
            case 0:
                bitsPerComponent = 32;
                break;
            case 1:
                bitsPerComponent = 16;
                break;
            case 2:
                bitsPerComponent = 8;
                break;
            case 3:
                bitsPerComponent = (vn == 3u) ? 4 : 16;
                break;
            default:
                break;
            }
            const int bitsPerVector = (vl == 3u && vn == 3u) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = static_cast<uint32_t>((bitsPerVector + 7) / 8);
            const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            uint32_t cl = vif0_regs.cycle & 0xFFu;
            uint32_t wl = (vif0_regs.cycle >> 8) & 0xFFu;
            if (cl == 0u)
                cl = 1u;
            if (wl == 0u)
                wl = 1u;
            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl)
            {
                const uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t remainder = writeVectorCount % wl;
                if (remainder > cl)
                    remainder = cl;
                sourceVectorCount = fullBlocks * cl + remainder;
            }
            uint32_t totalBytes = sourceVectorCount * bytesPerVector;
            totalBytes = (totalBytes + 3u) & ~3u;

            if (m_vu0Data && pos + totalBytes <= sizeBytes && vl == 0u)
            {
                uint32_t vuAddr = static_cast<uint32_t>(imm & 0x3FFu);
                if ((imm & 0x8000u) != 0u)
                    vuAddr = (vuAddr + (vif0_regs.tops & 0x3FFu)) & 0x3FFu;
                const uint8_t *srcBase = data + pos;
                uint32_t srcIndex = 0u;
                for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                {
                    const uint32_t cyclePos = writeIndex % wl;
                    const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);
                    uint32_t destVec = (cl >= wl) ? ((vuAddr + (writeIndex / wl) * cl + cyclePos) & 0x3FFu)
                                                  : ((vuAddr + writeIndex) & 0x3FFu);
                    const uint32_t destOff = destVec * 16u;
                    if (destOff + 16u > PS2_VU0_DATA_SIZE)
                    {
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                            ++srcIndex;
                        continue;
                    }
                    if (!sourceAvailable || srcIndex >= sourceVectorCount)
                        continue;
                    const uint8_t *srcVec = srcBase + srcIndex * bytesPerVector;
                    ++srcIndex;
                    uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                    std::memcpy(lanes, m_vu0Data + destOff, sizeof(lanes));
                    const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                    for (uint32_t c = 0; c < limit; ++c)
                    {
                        uint32_t scalar = 0u;
                        std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                        lanes[c] = scalar;
                    }
                    _mm_storeu_si128(reinterpret_cast<__m128i *>(m_vu0Data + destOff), _mm_loadu_si128(reinterpret_cast<const __m128i *>(lanes)));
                }
            }
            pos += totalBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else
        {
            break;
        }
    }
}

void PS2Memory::processVIF1Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (sizeBytes == 0u || srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF1Data(m_rdram + srcPhys, sizeBytes);
}

void PS2Memory::processVIF1Data(const uint8_t *data, uint32_t sizeBytes)
{
    if (sizeBytes == 0u)
        return;

    uint32_t pos = 0;

    // [DoD] DIRECT/DIRECTHL payload to the GIF (PATH2). The GIF keeps its tag state between DIRECT commands, so
    // IMAGE data announced by a tag in one DIRECT may arrive in the next DIRECT's payload: that data continues
    // the image behind a synthetic IMAGE tag. (The interpreter used to take whatever bytes followed in the VIF
    // stream, VIFcodes included, as the image, and then parsed the next DIRECT's pixels as GIF tags.)
    auto submitDirectPayload = [&](const uint8_t *payload, uint32_t qw, bool directHl)
    {
        // [DoD] Opt-in, bounded capture of the actual PATH2 stream before interpretation.
        // The caller creates a unique directory, so evidence never replaces an older run.
        static const char *const s_captureDir = std::getenv("DOD_GIF_CAPTURE_DIR"); // [DoD] read once: this runs per DIRECT
        if (const char *captureDir = s_captureDir)
        {
            static uint32_t captureIndex = 0;
            if (captureIndex < 2048u)
            {
                const uint32_t index = captureIndex++;
                char filename[1024];
                std::snprintf(filename, sizeof(filename), "%s/path2_%04u.bin", captureDir, index);
                if (FILE *file = std::fopen(filename, "wb"))
                {
                    std::fwrite(payload, 16u, qw, file);
                    std::fclose(file);
                }
                std::fprintf(stderr, "[gif-capture] %u pos=%u size=%u qw=%u pending=%u source=%08X hl=%d\n",
                    index, pos, sizeBytes, qw, m_vif1PendingPath2ImageQwc,
                    dodVif1SourceOf(data, static_cast<uint32_t>(payload - data)), directHl);
            }
        }
        while (qw != 0u && m_vif1PendingPath2ImageQwc != 0u)
        {
            const uint32_t chunkQw = std::min<uint32_t>(m_vif1PendingPath2ImageQwc, std::min<uint32_t>(qw, 0x7FFFu));
            std::vector<uint8_t> imagePacket(16u + static_cast<size_t>(chunkQw) * 16u, 0u);
            const uint64_t imageTag =
                static_cast<uint64_t>(chunkQw & 0x7FFFu) |
                ((m_vif1PendingPath2ImageQwc == chunkQw) ? (1ull << 15) : 0ull) |
                (static_cast<uint64_t>(kGifFmtImage) << 58);
            std::memcpy(imagePacket.data(), &imageTag, sizeof(imageTag));
            std::memcpy(imagePacket.data() + 16u, payload, static_cast<size_t>(chunkQw) * 16u);
            dodpcsx2::gif(2, payload, chunkQw * 16u); // [DoD] PCSX2's GS continues the tag itself
            {
                dodpcsx2::SuppressRaw synthetic;
                submitGifPacket(GifPathId::Path2, imagePacket.data(), static_cast<uint32_t>(imagePacket.size()), true, m_vif1PendingPath2DirectHl);
            }
            payload += static_cast<size_t>(chunkQw) * 16u;
            qw -= chunkQw;
            m_vif1PendingPath2ImageQwc -= chunkQw;
            if (m_vif1PendingPath2ImageQwc == 0u)
                m_vif1PendingPath2DirectHl = false;
        }
        if (qw == 0u)
            return;
        submitGifPacket(GifPathId::Path2, payload, qw * 16u, true, directHl);
        const uint32_t pendingImageQw = pendingGifImageQwc(payload, qw * 16u);
        if (pendingImageQw != 0u)
        {
            m_vif1PendingPath2ImageQwc = pendingImageQw;
            m_vif1PendingPath2DirectHl = directHl;
        }
    };

    while (pos + 4 <= sizeBytes)
    {
        dodVif1Ensure(data, pos + 4u);
        if (g_dodDirectLeftQw != 0u)
        {
            // the rest of a DIRECT whose payload was cut off at the end of the previous buffer
            const uint32_t availableQw = (sizeBytes - pos) / 16u;
            if (availableQw == 0u)
                break;
            const uint32_t chunkQw = std::min<uint32_t>(g_dodDirectLeftQw, availableQw);
            dodVif1Ensure(data, pos + chunkQw * 16u);
            submitDirectPayload(data + pos, chunkQw, g_dodDirectLeftHl);
            pos += chunkQw * 16u;
            g_dodDirectLeftQw -= chunkQw;
            continue;
        }

        uint32_t cmd;
        memcpy(&cmd, data + pos, 4);
        pos += 4;

        uint8_t opcode = (cmd >> 24) & 0x7F;
        ++g_dodVif1OpcodeCounts[opcode]; // [DoD] diagnostics for --trace
        g_dodVif1Recent[g_dodVif1RecentAt++ & 15u] = {pos - 4u, cmd, vif1_regs.cycle, data}; // [DoD] DOD_VIF_LOG
        uint16_t imm = cmd & 0xFFFF;
        uint8_t num = (cmd >> 16) & 0xFF;
        const bool irq = (cmd & 0x80000000u) != 0u;

        // Track most-recent command for VIFn_CODE emulation.
        vif1_regs.code = cmd;
        vif1_regs.num = num;
        if (irq)
            vif1_regs.stat |= (1u << 11); // INT

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif1_regs.cycle = imm;
            dodVif1Event("STCYCL", cmd, sizeBytes);
            continue;
        }
        else if (opcode == VIF_OFFSET)
        {
            // VIF double-buffer setup. OFFSET clears DBF and resets TOPS to BASE.
            // Do not rewrite BASE from the previous TOPS value.
            vif1_regs.ofst = imm & 0x3FFu;
            vif1_regs.tops = vif1_regs.base & 0x3FFu;
            vif1_regs.stat &= ~(1u << 7); // clear DBF
            continue;
        }
        else if (opcode == VIF_BASE)
        {
            // BASE only updates the base register. TOPS changes on OFFSET/MSCAL.
            vif1_regs.base = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            // ITOP VIFcode writes pending ITOPS; VU XITOP observes it after MSCAL/MSCNT.
            vif1_regs.itops = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif1_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MSKPATH3)
        {
            // VIF command docs: MSKPATH3 uses IMMEDIATE bit 15.
            const bool wasMasked = m_path3Masked;
            m_path3Masked = (imm & 0x8000u) != 0u;
            if (wasMasked && !m_path3Masked)
                flushMaskedPath3Packets();
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif1_regs.mark = imm;
            vif1_regs.stat |= (1u << 6); // MRK
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            continue;
        }
        else if (opcode == VIF_MSCAL || opcode == VIF_MSCALF)
        {
            uint32_t startPC = (uint32_t)imm * 8u;

            const uint32_t runTop = vif1_regs.tops & 0x3FFu;
            const uint32_t runItop = vif1_regs.itops & 0x3FFu;
            vif1_regs.top = runTop;
            vif1_regs.itop = runItop;

            const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
            if (dbf)
                vif1_regs.tops = vif1_regs.base & 0x3FFu;
            else
                vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF

            if (m_vu1MscalCallback)
                m_vu1MscalCallback(startPC, runTop, runItop);
            continue;
        }
        else if (opcode == VIF_MSCNT)
        {
            const uint32_t runTop = vif1_regs.tops & 0x3FFu;
            const uint32_t runItop = vif1_regs.itops & 0x3FFu;
            vif1_regs.top = runTop;
            vif1_regs.itop = runItop;

            const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
            if (dbf)
                vif1_regs.tops = vif1_regs.base & 0x3FFu;
            else
                vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF

            if (m_vu1MscntCallback)
                m_vu1MscntCallback(runTop, runItop);
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4 > sizeBytes)
                break;
            dodVif1Ensure(data, pos + 4u);
            uint32_t maskValue = 0;
            std::memcpy(&maskValue, data + pos, sizeof(maskValue));
            vif1_regs.mask = maskValue;
            pos += 4;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16 > sizeBytes)
                break;
            dodVif1Ensure(data, pos + 16u);
            std::memcpy(vif1_regs.row, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16 > sizeBytes)
                break;
            dodVif1Ensure(data, pos + 16u);
            std::memcpy(vif1_regs.col, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            uint32_t destAddr = (uint32_t)imm * 8u;
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            dodVif1Ensure(data, pos + mpgBytes);
            if (m_vu1Code && destAddr < PS2_VU1_CODE_SIZE && mpgBytes > 0)
            {
                uint32_t copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU1_CODE_SIZE)
                    copyBytes = PS2_VU1_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                {
                    std::memcpy(m_vu1Code + destAddr, data + pos, copyBytes);
                    markVU1CodeModified();
                }
            }
            if (pos + mpgBytes > sizeBytes)
                dodVif1Note("MPG cut off", cmd, pos, mpgBytes, sizeBytes);
            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if (opcode == VIF_DIRECT || opcode == VIF_DIRECTHL)
        {
            uint32_t qwCount = imm;
            if (qwCount == 0)
                qwCount = 65536;
            const uint32_t availableQw = (sizeBytes - pos) / 16u;
            const bool truncated = qwCount > availableQw;
            const bool directHl = (opcode == VIF_DIRECTHL);
            if (qwCount >= 4096u)
            {
                static int s_logged = 0; // [DoD] diagnostics: very large DIRECT and the VIF bytes around it
                if (s_logged++ < 3)
                {
                    dodVif1Ensure(data, std::min<uint32_t>(sizeBytes, pos + 32u));
                    std::string before, after;
                    char hex[4];
                    for (uint32_t i = pos >= 68u ? pos - 68u : 0u; i < pos; ++i)
                    {
                        std::snprintf(hex, sizeof(hex), "%02X", data[i]);
                        before += hex;
                        if ((i & 3u) == 3u)
                            before += ' ';
                    }
                    for (uint32_t i = pos; i < std::min<uint32_t>(sizeBytes, pos + 32u); ++i)
                    {
                        std::snprintf(hex, sizeof(hex), "%02X", data[i]);
                        after += hex;
                        if ((i & 3u) == 3u)
                            after += ' ';
                    }
                    std::fprintf(stderr, "[vif1] DIRECT%s %u qw at +%u of %u (code %08X) payload from 0x%08X\n  before: %s\n  after: %s\n",
                                 directHl ? "HL" : "", qwCount, pos - 4u, sizeBytes, cmd, dodVif1SourceOf(data, pos),
                                 before.c_str(), after.c_str());
                }
            }
            if (truncated)
            {
                g_dodDirectLeftQw = qwCount - availableQw; // continued by the next buffer (see loop top)
                g_dodDirectLeftHl = directHl;
                dodVif1Note("DIRECT continues in the next transfer", cmd, pos, qwCount * 16u, sizeBytes);
            }
            if (qwCount > availableQw)
                qwCount = availableQw;

            if (qwCount > 0)
            {
                dodVif1Ensure(data, pos + qwCount * 16u);
                submitDirectPayload(data + pos, qwCount, directHl);
            }

            pos += qwCount * 16;
            if (truncated)
            {
                pos = sizeBytes;
                break;
            }
            continue;
        }
        else if ((opcode & 0x60) == 0x60)
        {
            uint8_t vn = (opcode >> 2) & 0x3;
            uint8_t vl = opcode & 0x3;
            const bool maskEnable = (opcode & 0x10u) != 0u;
            int components = vn + 1;
            int bitsPerComponent = 32;
            switch (vl)
            {
            case 0:
                bitsPerComponent = 32;
                break;
            case 1:
                bitsPerComponent = 16;
                break;
            case 2:
                bitsPerComponent = 8;
                break;
            case 3:
                bitsPerComponent = (vn == 3) ? 4 : 16;
                break;
            default:
                break;
            }
            int bitsPerVector = (vl == 3 && vn == 3) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = (bitsPerVector + 7) / 8;
            // UNPACK semantics: NUM is 8-bit and NUM==0 means 256 vectors (writes).
            const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);

            // STCYCL controls write cycles for UNPACK.
            uint32_t cl = vif1_regs.cycle & 0xFFu;
            uint32_t wl = (vif1_regs.cycle >> 8) & 0xFFu;
            if (cl == 0u)
                cl = 1u;
            if (wl == 0u)
                wl = 1u;

            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl)
            {
                const uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t remainder = writeVectorCount % wl;
                if (remainder > cl)
                    remainder = cl;
                sourceVectorCount = fullBlocks * cl + remainder;
            }

            uint32_t totalBytes = sourceVectorCount * bytesPerVector;
            totalBytes = (totalBytes + 3) & ~3u;
            dodVif1Ensure(data, pos + totalBytes);

            uint32_t vuAddr = (uint32_t)imm & 0x3FFu;
            if ((imm & 0x8000u) != 0u)
                vuAddr = (vuAddr + (vif1_regs.tops & 0x3FFu)) & 0x3FFu;

            const bool zeroExtend = (imm & 0x4000u) != 0u;

            if (!dodLegacyVif1Unpack())
            {
                // [DoD] PCSX2's source size: fill mode reads CL vectors per WL block (WL = 0 means 256).
                const uint32_t pcl = vif1_regs.cycle & 0xFFu;
                const uint32_t pwl = ((vif1_regs.cycle >> 8) & 0xFFu) ? ((vif1_regs.cycle >> 8) & 0xFFu) : 256u;
                const uint32_t srcVectors = (pcl < pwl) ? pcl * (writeVectorCount / pwl) + std::min(writeVectorCount % pwl, pcl)
                                                        : writeVectorCount;
                const uint32_t srcBytes = (srcVectors * kVifGsize[opcode & 0xFu] + 3u) & ~3u;
                dodVif1Ensure(data, pos + srcBytes);
                if (m_vu1Data && pos + srcBytes <= sizeBytes)
                    vif1UnpackPcsx2(m_vu1Data, PS2_VU1_DATA_SIZE, vif1_regs, cmd, data + pos, srcBytes);
                else
                    dodVif1Note("UNPACK cut off", cmd, pos, srcBytes, sizeBytes);
                pos += srcBytes;
                if (pos > sizeBytes)
                    break;
                continue;
            }

            if (m_vu1Data && totalBytes > 0 && pos + totalBytes <= sizeBytes)
            {
                const uint8_t *srcBase = data + pos;
                uint32_t srcIndex = 0u;
                for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                {
                    const uint32_t cyclePos = writeIndex % wl;
                    const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);

                    uint32_t destVec = 0;
                    if (cl >= wl)
                    {
                        destVec = (vuAddr + (writeIndex / wl) * cl + cyclePos) & 0x3FFu;
                    }
                    else
                    {
                        destVec = (vuAddr + writeIndex) & 0x3FFu;
                    }

                    uint32_t destOff = destVec * 16u;
                    if (destOff + 16u > PS2_VU1_DATA_SIZE)
                    {
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                            ++srcIndex;
                        continue;
                    }

                    uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                    std::memcpy(lanes, m_vu1Data + destOff, sizeof(lanes));
                    uint32_t decompressed[4] = {lanes[0], lanes[1], lanes[2], lanes[3]};
                    bool decoded = false;

                    const uint8_t *srcVec = nullptr;
                    if (sourceAvailable && srcIndex < sourceVectorCount)
                    {
                        srcVec = srcBase + srcIndex * bytesPerVector;
                        ++srcIndex;
                        decoded = true;
                    }

                    auto extend16 = [&](uint16_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
                    };

                    auto extend8 = [&](uint8_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(raw)));
                    };

                    bool handledFormat = true;
                    if (!decoded)
                    {
                        handledFormat = false;
                    }
                    else if (vl == 0u)
                    {
                        if (components == 1)
                        {
                            uint32_t scalar = 0;
                            std::memcpy(&scalar, srcVec, sizeof(scalar));
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint32_t scalar = 0;
                                std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                                decompressed[c] = scalar;
                            }
                        }
                    }
                    else if (vl == 1u)
                    {
                        if (components == 1)
                        {
                            uint16_t raw = 0;
                            std::memcpy(&raw, srcVec, sizeof(raw));
                            const uint32_t scalar = extend16(raw);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint16_t raw = 0;
                                std::memcpy(&raw, srcVec + c * 2u, sizeof(raw));
                                decompressed[c] = extend16(raw);
                            }
                        }
                    }
                    else if (vl == 2u)
                    {
                        if (components == 1)
                        {
                            const uint32_t scalar = extend8(srcVec[0]);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                decompressed[c] = extend8(srcVec[c]);
                            }
                        }
                    }
                    else if (vl == 3u && vn == 3u)
                    {
                        // V4-5: packed color-like format in a single 16-bit value.
                        uint16_t packed = 0;
                        std::memcpy(&packed, srcVec, sizeof(packed));
                        decompressed[0] = packed & 0x1Fu;
                        decompressed[1] = (packed >> 5) & 0x1Fu;
                        decompressed[2] = (packed >> 10) & 0x1Fu;
                        decompressed[3] = (packed >> 15) & 0x01u;
                    }
                    else
                    {
                        handledFormat = false;
                    }

                    // Unknown compressed format fallback: preserve legacy raw-copy behavior.
                    if (!handledFormat && decoded && !maskEnable && (vif1_regs.mode == 0u || vif1_regs.mode == 3u))
                    {
                        uint32_t copyBytes = (bytesPerVector < 16u) ? bytesPerVector : 16u;
                        std::memcpy(m_vu1Data + destOff, srcVec, copyBytes);
                        continue;
                    }

                    const bool canAdd = (vl != 3u || vn != 3u);
                    const uint32_t mode = vif1_regs.mode & 3u;
                    const uint32_t colIdx = (cyclePos > 3u) ? 3u : cyclePos;
                    const uint32_t maskCycle = (cyclePos > 3u) ? 3u : cyclePos;

                    for (uint32_t field = 0u; field < 4u; ++field)
                    {
                        uint32_t maskSpec = 0u;
                        if (maskEnable)
                        {
                            const uint32_t shift = ((maskCycle * 4u) + field) * 2u;
                            maskSpec = (vif1_regs.mask >> shift) & 0x3u;
                        }

                        // In fill-write cycles with suspended source reads, treat raw-data selections as row-fill.
                        if (!decoded && maskSpec == 0u)
                            maskSpec = 1u;

                        uint32_t writeVal = lanes[field];
                        if (maskSpec == 0u)
                        {
                            if (handledFormat)
                            {
                                writeVal = decompressed[field];
                                if (canAdd && (mode == 1u || mode == 2u))
                                {
                                    writeVal = writeVal + vif1_regs.row[field];
                                    if (mode == 2u)
                                        vif1_regs.row[field] = writeVal;
                                }
                            }
                        }
                        else if (maskSpec == 1u)
                        {
                            writeVal = vif1_regs.row[field];
                        }
                        else if (maskSpec == 2u)
                        {
                            writeVal = vif1_regs.col[colIdx];
                        }
                        else
                        {
                            continue; // write-protect
                        }

                        lanes[field] = writeVal;
                    }

                    std::memcpy(m_vu1Data + destOff, lanes, sizeof(lanes));
                }
            }
            pos += totalBytes;

            if (pos > sizeBytes)
                break;
            continue;
        }
        else
        {
            // [DoD] As the VIF (PCSX2 Vif_Codes.cpp vifCode_Null): an undefined VIFcode sets STAT.ER1 and stops the VIF
            // unless VIF1_ERR.ME1 masks the error. Skipping it and going on ran whatever followed (DMA tags, VU code,
            // vertices) as VIFcodes; a stray DIRECT then sent megabytes of it to the GS (room 0x2CD: heap corruption,
            // PCSX2 GS crash). The rest of this transfer is dropped.
            dodVif1Note("unknown command", cmd, pos, 0u, sizeBytes);
            if ((vif1_regs.err & 0x4u) == 0u)
            {
                static int s_reported = 0;
                if (s_reported++ < 5)
                    std::fprintf(stderr, "[vif1] invalid VIFcode %08X at +%u of %u: transfer stopped (STAT.ER1)\n", cmd, pos - 4u,
                                 sizeBytes);
                vif1_regs.stat |= (1u << 13); // ER1
                g_dodDirectLeftQw = 0u;
                return;
            }
            continue;
        }
    }
}
