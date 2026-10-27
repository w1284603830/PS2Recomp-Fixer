#include "runtime/gs/ps2_gif_arbiter.h"
#include "dod_pcsx2_gs.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

GifArbiter::GifArbiter(ProcessPacketFn processFn)
    : m_processFn(std::move(processFn))
{
}

bool GifArbiter::isImagePacket(const uint8_t *data, uint32_t sizeBytes)
{
    if (!data || sizeBytes < 16u)
        return false;

    uint64_t tagLo = 0;
    std::memcpy(&tagLo, data, sizeof(tagLo));
    const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
    return flg == 2u;
}

namespace
{
    // [DoD] With PCSX2's GS active it renders everything and serves local->host reads itself; the built-in frontend
    // only has to see the packets that change state the game reads back through it: A+D writes to SIGNAL / FINISH /
    // LABEL (CSR bits and the SIGNAL interrupt the IPU logo decode runs on) and TRXDIR with XDIR = 1 (a GS -> host
    // transfer, served through consumeLocalToHostBytes). Everything else (uploads, CLUT loads, draws) it used to
    // parse and copy into its own VRAM for nothing: ~13% of the game thread in gameplay. KillzoneRecomp patch 0012
    // ("exclusive host GS") does the same. DOD_GS_BUILTIN=1 keeps feeding the built-in frontend every packet.
    bool dodBuiltinGsNeedsPacket(const uint8_t *data, uint32_t size)
    {
        const uint32_t qwords = size / 16u;
        uint32_t q = 0;
        while (q < qwords)
        {
            uint64_t lo, hi;
            std::memcpy(&lo, data + q * 16u, 8);
            std::memcpy(&hi, data + q * 16u + 8u, 8);
            ++q;
            const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu);
            const uint32_t flg = static_cast<uint32_t>((lo >> 58) & 3u);
            uint32_t nreg = static_cast<uint32_t>((lo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;
            if (flg == 0u)
            {
                for (uint32_t l = 0; l < nloop; ++l)
                    for (uint32_t r = 0; r < nreg; ++r, ++q)
                    {
                        if (q >= qwords)
                            return false;
                        if (((hi >> (4u * r)) & 0xFu) != 0xEu)
                            continue;
                        const uint8_t addr = data[q * 16u + 8u];
                        if (addr >= 0x60u && addr <= 0x62u)
                            return true;
                        if (addr == 0x53u && (data[q * 16u] & 3u) == 1u)
                            return true;
                    }
            }
            else if (flg == 1u)
                q += (nloop * nreg + 1u) / 2u;
            else
                q += nloop;
        }
        return false;
    }
}

void GifArbiter::submit(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool path2DirectHl)
{
    if (!data || sizeBytes < 16 || !m_processFn)
        return;
    static const bool builtinAll = std::getenv("DOD_GS_BUILTIN") != nullptr;
    if (!builtinAll && dodpcsx2::active() && !dodBuiltinGsNeedsPacket(data, sizeBytes))
        return;

    GifArbiterPacket pkt;
    pkt.pathId = pathId;
    pkt.path2DirectHl = (pathId == GifPathId::Path2) && path2DirectHl;
    pkt.path3Image = (pathId == GifPathId::Path3) && isImagePacket(data, sizeBytes);
    pkt.data.resize(sizeBytes);
    std::memcpy(pkt.data.data(), data, sizeBytes);
    m_queue.push_back(std::move(pkt));
}

void GifArbiter::drain()
{
    if (!m_processFn)
        return;

    // [DoD] Upstream PR #299: packets go out in submission order. Path priority only decides which path gets the
    // bus when several request it at the same instant; the runtime performs each transfer when it is started, so
    // packets queued between two drains were issued at different times (a PATH3 texture upload sorted behind the
    // PATH1 draws that use it garbled textures). DOD_GIF_PRIORITY_SORT=1: the old sort.
    static const bool prioritySort = std::getenv("DOD_GIF_PRIORITY_SORT") != nullptr;
    if (prioritySort)
        std::stable_sort(m_queue.begin(), m_queue.end(),
                         [](const GifArbiterPacket &a, const GifArbiterPacket &b)
                         {
                             // DIRECTHL cannot preempt PATH3 IMAGE transfers.
                             if (a.path2DirectHl != b.path2DirectHl || a.path3Image != b.path3Image)
                             {
                                 if (a.path3Image && b.path2DirectHl)
                                     return true;
                                 if (a.path2DirectHl && b.path3Image)
                                     return false;
                             }
                             return pathPriority(a.pathId) < pathPriority(b.pathId);
                         });

    for (size_t i = 0; i < m_queue.size(); ++i)
    {
        auto &pkt = m_queue[i];
        if (!pkt.data.empty())
        {
            m_processFn(pkt.data.data(), static_cast<uint32_t>(pkt.data.size()));
        }
    }
    m_queue.clear();
}

uint8_t GifArbiter::pathPriority(GifPathId id)
{
    return static_cast<uint8_t>(id);
}
