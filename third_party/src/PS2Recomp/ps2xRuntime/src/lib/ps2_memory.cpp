#include "runtime/ps2_memory.h"
#include "runtime/ps2_address.h"
#include "runtime/gs/gs_frontend.h"
#include "ps2_log.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <algorithm>
#include <string>
#include <vector>

#include "ps2_ipu.h"
#include "gs/dod_pcsx2_gs.h"
#include "ipu/ps2_ipu_compat.h"
#include "dod_to_ipu_dma.h"

#include <unordered_map>
#include <chrono>
#include <map>
#include <mutex>

uint32_t dodLastGuestPc(); // [DoD] ps2_runtime.cpp diagnostics
uint32_t dodLastGuestRa();

uint64_t g_dodDmaStarts[64][2] = {}; // [DoD] diagnostics for --trace: DMA starts by (channel base >> 10) & 63, CHCR.DIR
uint64_t g_dodIpuRegisterWrites = 0;

// [DoD] DOD_IO_CENSUS=<file> (diagnostics, offline test of the hardware registers): every EE I/O register (0x1000xxxx),
// GS privileged register (0x1200xxxx) and FIFO the game reads or writes, with counts, the OR / AND of the written
// values (which bits it uses), the last value and the first and last guest pc. Written as CSV every 20 s and at exit.
namespace dodio
{
    struct Entry
    {
        uint64_t reads = 0u, writes = 0u, orBits = 0u, andBits = ~0ull, lastWrite = 0u, lastRead = 0u;
        uint32_t firstPc = 0u, lastPc = 0u;
    };

    struct Census
    {
        std::mutex mutex;
        std::map<uint32_t, Entry> entries;
        std::string path;
        std::chrono::steady_clock::time_point lastDump = std::chrono::steady_clock::now();
        uint32_t sinceCheck = 0u;

        void dump()
        {
            std::FILE *f = std::fopen(path.c_str(), "w");
            if (!f)
                return;
            std::fprintf(f, "address,reads,writes,or_bits,and_bits,last_write,last_read,first_pc,last_pc\n");
            for (const auto &[addr, e] : entries)
                std::fprintf(f, "%08X,%llu,%llu,%016llX,%016llX,%016llX,%016llX,%08X,%08X\n", addr,
                             static_cast<unsigned long long>(e.reads), static_cast<unsigned long long>(e.writes),
                             static_cast<unsigned long long>(e.orBits), static_cast<unsigned long long>(e.writes ? e.andBits : 0u),
                             static_cast<unsigned long long>(e.lastWrite), static_cast<unsigned long long>(e.lastRead), e.firstPc, e.lastPc);
            std::fclose(f);
        }

        ~Census()
        {
            std::lock_guard<std::mutex> lock(mutex);
            dump();
        }
    };

    Census *census()
    {
        static Census *const c = []() -> Census * {
            const char *v = std::getenv("DOD_IO_CENSUS");
            if (!v || !*v)
                return nullptr;
            static Census instance;
            instance.path = v;
            return &instance;
        }();
        return c;
    }

    void note(uint32_t address, bool write, uint64_t value)
    {
        Census *c = census();
        if (!c)
            return;
        std::lock_guard<std::mutex> lock(c->mutex);
        Entry &e = c->entries[address];
        const uint32_t pc = dodLastGuestPc();
        if (e.reads + e.writes == 0u)
            e.firstPc = pc;
        e.lastPc = pc;
        if (write)
        {
            ++e.writes;
            e.orBits |= value;
            e.andBits &= value;
            e.lastWrite = value;
        }
        else
        {
            ++e.reads;
            e.lastRead = value;
        }
        if (++c->sinceCheck >= 4096u)
        {
            c->sinceCheck = 0u;
            const auto now = std::chrono::steady_clock::now();
            if (now - c->lastDump >= std::chrono::seconds(20))
            {
                c->lastDump = now;
                c->dump();
            }
        }
    }
}

namespace
{
    // [DoD] A DMA address with bit 31 set selects the scratchpad (bits 0-13); 0x70000000 is the CPU's scratchpad.
    bool dodDmaAddressIsScratchpad(uint32_t address)
    {
        return (address & 0x80000000u) != 0u || (address & 0xF0000000u) == 0x70000000u;
    }

    // [DoD] fromIPU (DMA channel 3) transfer waiting for the IPU to produce output.
    struct DodIpuFromTransfer
    {
        bool active = false;
        uint32_t madr = 0;
        uint32_t qwc = 0;
    };
    DodIpuFromTransfer g_dodIpuFrom;
    dod_mpeg_dma::PendingTransfer g_dodMpegToIpu;

    bool dodServiceMpegToIpu(std::unordered_map<uint32_t, uint32_t> &io)
    {
        if (!g_dodMpegToIpu.active()) return false;
        // Explicit guest STR clear is a DMA stop, never a successful completion.
        if ((io[0x1000B400u] & 0x100u) == 0u)
        { g_dodMpegToIpu.abort(); return false; }
        const auto control = io.find(0x1000E000u);
        if (control != io.end() && (control->second & 1u) == 0u) return false;
        const auto session = g_dodMpegToIpu.session();
        const size_t bytes = g_dodMpegToIpu.bytes();
        const auto result = g_dodMpegToIpu.service(dod_mpeg_dma::ingress(), io);
        static unsigned logged = 0;
        if (logged < 32u && result != dod_mpeg_dma::SubmitResult::Backpressure)
        {
            std::fprintf(stderr, "[MPEG:DMA] mpeg=%08X generation=%llu bytes=%zu result=%u qwc=%u\n",
                         session.mpegAddr, static_cast<unsigned long long>(session.generation), bytes,
                         static_cast<unsigned>(result), io[0x1000B420u]);
            ++logged;
        }
        // A stale lifecycle or oversized transfer stays stopped on the same
        // guest-visible position until the guest explicitly aborts it. No EOS.
        return result == dod_mpeg_dma::SubmitResult::Accepted;
    }

    // [DoD] VIF1 DMA chains are walked when the channel starts, but on hardware the DMA reads each tag's data only
    // when it gets there. The data is copied from memory lazily (see dodVif1Ensure) so that work done between
    // (a GS SIGNAL interrupt that decodes the next rows into the buffer the following tags send) is seen.
    struct DodChainSegment
    {
        uint32_t offset = 0; // in the chain buffer
        uint32_t size = 0;
        uint32_t source = 0; // physical RAM or scratchpad offset
        bool scratchpad = false;
        bool literal = false; // tag bytes captured at walk time (TTE)
    };
    struct DodLazyChain
    {
        std::vector<DodChainSegment> segments;
        size_t materialized = 0; // segments copied so far
        uint8_t *buffer = nullptr;
        uint8_t *rdram = nullptr;
        uint8_t *scratchpad = nullptr;
    };
    std::unordered_map<const uint8_t *, DodLazyChain> g_dodLazyChains;

    // [DoD] DMA chain buffers are recycled (KillzoneRecomp patch 0016 "chain buffer pool"): a gameplay frame's VIF1 chain
    // is a few MB, and a fresh vector per DMA grown tag by tag paid the reallocations and page faults every frame.
    // EE thread only. DOD_CHAIN_POOL=0: a fresh vector each time.
    std::vector<std::vector<uint8_t>> g_dodChainPool;

    std::vector<uint8_t> dodChainAcquire()
    {
        static const bool pool = [] { const char *v = std::getenv("DOD_CHAIN_POOL"); return !v || *v != '0'; }();
        if (!pool || g_dodChainPool.empty())
            return {};
        std::vector<uint8_t> buffer = std::move(g_dodChainPool.back());
        g_dodChainPool.pop_back();
        buffer.clear();
        return buffer;
    }

    void dodChainRecycle(std::vector<uint8_t> &&buffer)
    {
        if (buffer.capacity() != 0u && g_dodChainPool.size() < 8u)
            g_dodChainPool.push_back(std::move(buffer));
    }
    DodLazyChain *g_dodActiveVif1Chain = nullptr;
    bool g_dodInPendingTransfers = false;
    uint32_t g_dodPath3PendingImageQwc = 0; // [DoD] IMAGE data still owed by the last PATH3 GIF tag

    // Quadwords of IMAGE data the last tag of a GIF packet still expects beyond the packet (FLG 2 and 3).
    uint32_t dodPendingGifImageQwc(const uint8_t *data, uint32_t sizeBytes)
    {
        uint32_t offset = 0u;
        while (offset + 16u <= sizeBytes)
        {
            uint64_t tagLo = 0u;
            std::memcpy(&tagLo, data + offset, sizeof(tagLo));
            offset += 16u;
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint32_t flg = static_cast<uint32_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;
            uint64_t payload = 0u;
            if (flg == 0u)
                payload = static_cast<uint64_t>(nloop) * nreg * 16u;
            else if (flg == 1u)
                payload = ((static_cast<uint64_t>(nloop) * nreg * 8u) + 15u) & ~15ull;
            else
            {
                payload = static_cast<uint64_t>(nloop) * 16u;
                const uint64_t available = sizeBytes - offset;
                if (payload > available)
                    return static_cast<uint32_t>((payload - available) / 16u);
            }
            if (payload > sizeBytes - offset)
                return 0u; // PACKED/REGLIST split across transfers: not continued
            offset += static_cast<uint32_t>(payload);
        }
        return 0u;
    }

    uint8_t *dodDmaPointer(uint32_t address, uint8_t *rdram, uint8_t *scratchpad)
    {
        if (dodDmaAddressIsScratchpad(address))
            return scratchpad + (address & (PS2_SCRATCHPAD_SIZE - 16u));
        return rdram + (address & (PS2_RAM_SIZE - 16u));
    }

    uint32_t dodDmaAdvance(uint32_t address, uint32_t bytes)
    {
        if (dodDmaAddressIsScratchpad(address))
            return (address & ~(PS2_SCRATCHPAD_SIZE - 1u)) | ((address + bytes) & (PS2_SCRATCHPAD_SIZE - 1u));
        return address + bytes;
    }

    // Moves IPU output into the pending fromIPU transfer; true when that transfer has completed.
    bool dodServiceIpuFrom(std::unordered_map<uint32_t, uint32_t> &io, uint8_t *rdram, uint8_t *scratchpad)
    {
        DodIpuFromTransfer &t = g_dodIpuFrom;
        if (!t.active)
            return false;
        while (t.qwc > 0u && ps2_ipu::outputBytes() >= 16u)
        {
            ps2_ipu::popOutput(dodDmaPointer(t.madr, rdram, scratchpad), 16u);
            t.madr = dodDmaAdvance(t.madr, 16u);
            --t.qwc;
        }
        io[0x1000B010u] = t.madr;
        io[0x1000B020u] = t.qwc;
        if (t.qwc != 0u)
            return false;
        t.active = false;
        return true;
    }

    // Gathers what a toIPU (channel 4) transfer sends: QWC from MADR, then in chain mode the tag chain from TADR.
    std::vector<uint8_t> dodGatherToIpu(std::unordered_map<uint32_t, uint32_t> &io, uint32_t chcr, uint8_t *rdram, uint8_t *scratchpad)
    {
        auto transfer = dod_mpeg_dma::gather(dod_mpeg_dma::readRegisters(io), chcr,
            [&](uint32_t address) { return dodDmaPointer(address, rdram, scratchpad); }, dodDmaAdvance);
        dod_mpeg_dma::commitRegisters(io, transfer.final);
        return std::move(transfer.bytes);
    }
}

// [DoD] IPU emulation from the Ace Combat 5 static recompilation (src/lib/ipu/ps2_ipu.c, Apache-2.0, via the Urban
// Reign port, which wrote this DMA side): every command (BCLR/IDEC/BDEC/VDEC/FDEC/SETIQ/SETVQ/CSC/PACK/SETTH) runs
// when it is written, pulls toIPU (channel 4) data as it needs it, and its output leaves through fromIPU (channel 3).
// It lets the game's own libmpeg (CRI Sofdec's decoder) drive the IPU (sceMpeg*/sceIpu* run natively, not as HLE
// stubs: work/recomp/dod_native_ipu.toml). DOD_IPU=legacy keeps ps2_ipu.cpp.
namespace dodac5ipu
{
    constexpr uint32_t kFromIpuChannel = 0x1000B000u;
    constexpr uint32_t kToIpuChannel = 0x1000B400u;
    PS2Memory *g_memory = nullptr;
    uint32_t g_pullLeft = 0u;
    bool g_tagEnd = false;

    bool enabled()
    {
        static const bool on = []
        {
            const char *value = std::getenv("DOD_IPU");
            return !(value && std::strcmp(value, "legacy") == 0);
        }();
        return on;
    }

    void attach(PS2Memory &memory)
    {
        static bool initialized = false;
        if (!initialized)
        {
            initialized = true;
            ps2_ipu_init();
        }
        g_memory = &memory;
    }

    // Host pointer for a DMA address (MADR/TADR; bit 31 = scratchpad) and how many bytes follow it in that memory.
    uint8_t *pointer(PS2Memory &memory, uint32_t address, uint32_t &room)
    {
        room = 0u;
        if ((address & 0x80000000u) != 0u || (address >= 0x70000000u && address < 0x70004000u))
        {
            const uint32_t offset = address & (PS2_SCRATCHPAD_SIZE - 1u) & ~15u;
            room = PS2_SCRATCHPAD_SIZE - offset;
            return memory.getScratchpad() + offset;
        }
        const uint32_t offset = address & 0x01FFFFF0u;
        if (offset >= PS2_RAM_SIZE)
            return nullptr;
        room = PS2_RAM_SIZE - offset;
        return memory.getRDRAM() + offset;
    }

    // toIPU: feeds the IPU up to g_pullLeft qwords (and only what its FIFO takes), walking the DMA chain in chain
    // mode; the channel ends at the end of the chain.
    void toAdvance(PS2Memory &memory)
    {
        auto &regs = memory.m_ioRegisters;
        uint32_t &chcr = regs[kToIpuChannel];
        uint32_t &madr = regs[kToIpuChannel + 0x10u];
        uint32_t &qwc = regs[kToIpuChannel + 0x20u];
        uint32_t &tadr = regs[kToIpuChannel + 0x30u];
        uint32_t tags = 0u;
        while ((chcr & 0x100u) != 0u)
        {
            if ((qwc & 0xFFFFu) != 0u)
            {
                uint32_t space = ps2_ipu_in_space();
                if (space > g_pullLeft)
                    space = g_pullLeft;
                if (space == 0u)
                    return;
                uint32_t n = std::min(qwc & 0xFFFFu, space);
                uint32_t room = 0u;
                const uint8_t *src = pointer(memory, madr, room);
                if (room / 16u != 0u && n > room / 16u)
                    n = room / 16u;
                if (!src || n == 0u)
                {
                    qwc = 0u;
                    continue;
                }
                ps2_ipu_dma_in(src, n);
                madr += n * 16u;
                qwc -= n;
                g_pullLeft -= n;
                continue;
            }
            if (((chcr >> 2) & 3u) != 1u || g_tagEnd || ++tags > 4096u)
                break;
            uint32_t room = 0u;
            const uint8_t *tagBytes = pointer(memory, tadr, room);
            if (!tagBytes || room < 16u)
                break;
            uint64_t tag = 0u;
            std::memcpy(&tag, tagBytes, sizeof(tag));
            qwc = static_cast<uint32_t>(tag & 0xFFFFu);
            const uint32_t id = static_cast<uint32_t>((tag >> 28) & 7u);
            const bool irq = ((tag >> 31) & 1u) != 0u;
            const uint32_t addr = static_cast<uint32_t>((tag >> 32) & 0xFFFFFFF0u);
            {
                // [DoD] DOD_IPU_TAGLOG=<n>: the first n toIPU tags.
                static int tagLog = []() { const char *v = std::getenv("DOD_IPU_TAGLOG"); return v ? std::atoi(v) : 0; }();
                if (tagLog > 0)
                {
                    --tagLog;
                    std::fprintf(stderr, "[ipu-tag] tadr=%08X tag=%016llX id=%u qwc=%u addr=%08X irq=%d chcr=%08X\n", tadr,
                                 static_cast<unsigned long long>(tag), id, qwc, addr, irq ? 1 : 0, chcr);
                }
            }
            if (addr == 0u && qwc == 0u && id == 0u)
            {
                g_tagEnd = true; // an empty tag: the chain already ended
                break;
            }
            switch (id)
            {
            case 0: // refe
                madr = addr;
                tadr += 16u;
                g_tagEnd = true;
                break;
            case 1: // cnt
                madr = tadr + 16u;
                tadr += 16u + qwc * 16u;
                break;
            case 2: // next
                madr = tadr + 16u;
                tadr = addr;
                break;
            case 3: // ref
            case 4: // refs
                madr = addr;
                tadr += 16u;
                break;
            case 7: // end
                madr = tadr + 16u;
                g_tagEnd = true;
                break;
            default:
                qwc = 0u;
                g_tagEnd = true;
                break;
            }
            if (irq && ((chcr >> 7) & 1u) != 0u)
                g_tagEnd = true; // TIE: the tag's IRQ bit ends the transfer
        }
        if ((chcr & 0x100u) == 0u)
            return;
        {
            static int endLog = []() { const char *v = std::getenv("DOD_IPU_TAGLOG"); return v ? std::atoi(v) : 0; }();
            if (endLog > 0)
            {
                --endLog;
                std::fprintf(stderr, "[ipu-tag] toIPU complete tadr=%08X madr=%08X tagEnd=%d tags=%u\n", tadr, madr,
                             g_tagEnd ? 1 : 0, tags);
            }
        }
        g_tagEnd = false;
        memory.completeDmacChannel(kToIpuChannel, 4u);
    }

    // fromIPU: moves the decoded output to memory; the channel ends when its QWC is done.
    void fromAdvance(PS2Memory &memory)
    {
        auto &regs = memory.m_ioRegisters;
        uint32_t &chcr = regs[kFromIpuChannel];
        uint32_t &madr = regs[kFromIpuChannel + 0x10u];
        uint32_t &qwc = regs[kFromIpuChannel + 0x20u];
        while ((chcr & 0x100u) != 0u && (qwc & 0xFFFFu) != 0u)
        {
            const uint32_t avail = ps2_ipu_out_avail();
            if (avail == 0u)
                return;
            uint32_t n = std::min(qwc & 0xFFFFu, avail);
            uint32_t room = 0u;
            uint8_t *dst = pointer(memory, madr, room);
            if (room / 16u != 0u && n > room / 16u)
                n = room / 16u;
            if (!dst || n == 0u)
            {
                qwc = 0u;
                break;
            }
            ps2_ipu_dma_out(dst, n);
            if ((madr & 0x80000000u) == 0u)
                memory.markModified(madr & 0x01FFFFF0u, n * 16u);
            madr += n * 16u;
            qwc -= n;
        }
        if ((chcr & 0x100u) == 0u || (qwc & 0xFFFFu) != 0u)
            return;
        memory.completeDmacChannel(kFromIpuChannel, 3u);
    }
}

extern "C" void ps2_dmac_ipu_pull(void)
{
    if (!dodac5ipu::g_memory)
        return;
    const uint32_t want = ps2_ipu_want();
    dodac5ipu::g_pullLeft = want > 8u ? want : 8u;
    dodac5ipu::toAdvance(*dodac5ipu::g_memory);
    dodac5ipu::g_pullLeft = 0u;
}

extern "C" void ps2_dmac_ipu_drain(void)
{
    if (dodac5ipu::g_memory)
        dodac5ipu::fromAdvance(*dodac5ipu::g_memory);
}

extern "C" void ps2_dmac_ipu_report(u32 *chcr, u32 *madr, u32 *qwc, u32 *tadr)
{
    if (!dodac5ipu::g_memory)
    {
        *chcr = *madr = *qwc = *tadr = 0u;
        return;
    }
    auto &regs = dodac5ipu::g_memory->m_ioRegisters;
    *chcr = regs[dodac5ipu::kToIpuChannel];
    *madr = regs[dodac5ipu::kToIpuChannel + 0x10u];
    *qwc = regs[dodac5ipu::kToIpuChannel + 0x20u];
    *tadr = regs[dodac5ipu::kToIpuChannel + 0x30u];
}

extern "C" void ps2_dmac_ipu_history(void) {}
extern "C" void ps2_dmac_ipu_out_history(void) {}

// [DoD] diagnostics: the guest source address of chain byte `offset` (0xFFFFFFFF when unknown).
uint32_t dodVif1SourceOf(const uint8_t *data, uint32_t offset)
{
    const DodLazyChain *chain = g_dodActiveVif1Chain;
    if (!chain || chain->buffer != data)
        return 0xFFFFFFFFu;
    for (const DodChainSegment &seg : chain->segments)
    {
        if (offset >= seg.offset && offset < seg.offset + seg.size)
            return seg.literal ? 0xFFFFFFFEu : (seg.scratchpad ? 0x70000000u : 0u) + seg.source + (offset - seg.offset);
    }
    return 0xFFFFFFFFu;
}

void dodVif1ResetCommand(); // [DoD] ps2_vif1_interpreter.cpp: VIF1_FBRST.RST drops the command in progress

// [DoD] diagnostics (DOD_VIF_LOG / DOD_CHAIN_DUMP): the tags of the last VIF1 chain walked, with the chain buffer
// offset of each tag's VIF words; dodVif1ChainNotesAround prints the ones around a chain buffer offset.
struct DodTagNote { uint32_t at, id, qwc, addr, hi0, hi1, asp, off; };
std::vector<DodTagNote> g_dodLastVif1ChainNotes;
void dodVif1ChainNotesAround(uint32_t offset)
{
    const auto &notes = g_dodLastVif1ChainNotes;
    size_t k = 0;
    while (k < notes.size() && notes[k].off <= offset)
        ++k;
    const size_t first = k >= 12u ? k - 12u : 0u;
    const size_t last = std::min(notes.size(), k + 3u);
    for (size_t i = first; i < last; ++i)
    {
        const DodTagNote &t = notes[i];
        std::fprintf(stderr, "[vif1]   chain tag #%zu at +%u: tag@%08X lo=%08X qwc=%u addr=%08X vif %08X %08X asp %u%s" "\n", i, t.off, t.at,
                     t.id, t.qwc, t.addr, t.hi0, t.hi1, t.asp, (i + 1u == k) ? "  <- holds the offset" : "");
    }
}
void dodVif1Event(const char *what, uint32_t value, uint32_t extra); // [DoD] ps2_vif1_interpreter.cpp: DOD_VIF_EVENTS

// Called by the VIF1 interpreter before it uses chain bytes [0, endOffset).
void dodVif1Ensure(const uint8_t *data, uint32_t endOffset)
{
    DodLazyChain *chain = g_dodActiveVif1Chain;
    if (!chain || chain->buffer != data)
        return;
    while (chain->materialized < chain->segments.size())
    {
        const DodChainSegment &seg = chain->segments[chain->materialized];
        if (seg.offset >= endOffset)
            break;
        if (!seg.literal)
        {
            const uint8_t *base = seg.scratchpad ? chain->scratchpad : chain->rdram;
            const uint32_t limit = seg.scratchpad ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
            uint32_t src = seg.source;
            uint32_t copied = 0;
            while (copied < seg.size)
            {
                if (src >= limit)
                    src = 0;
                const uint32_t chunk = std::min(seg.size - copied, limit - src);
                std::memcpy(chain->buffer + seg.offset + copied, base + src, chunk);
                copied += chunk;
                src += chunk;
            }
        }
        ++chain->materialized;
    }
}

namespace
{
    inline void inRange(uint32_t offset, size_t bytes, size_t regionSize, const char *op, uint32_t address)
    {
        if (static_cast<uint64_t>(offset) + static_cast<uint64_t>(bytes) > static_cast<uint64_t>(regionSize))
        {
            throw std::runtime_error(std::string(op) + " out-of-bounds at address: 0x" + std::to_string(address));
        }
    }

    template <typename T>
    inline T loadScalar(const uint8_t *base, uint32_t offset, size_t regionSize, const char *op, uint32_t address)
    {
        inRange(offset, sizeof(T), regionSize, op, address);
        T value{};
        std::memcpy(&value, base + offset, sizeof(T));
        return value;
    }

    template <typename T>
    inline void storeScalar(uint8_t *base, uint32_t offset, size_t regionSize, T value, const char *op, uint32_t address)
    {
        inRange(offset, sizeof(T), regionSize, op, address);
        std::memcpy(base + offset, &value, sizeof(T));
    }

    inline bool isGsPrivReg(uint32_t addr)
    {
        return Ps2AddressInRange(addr, PS2_GS_PRIV_REG_BASE, PS2_GS_PRIV_REG_SIZE);
    }

    inline bool isIoRegister(uint32_t addr)
    {
        return Ps2AddressInRange(addr, PS2_IO_BASE, PS2_IO_SIZE);
    }

    inline uint64_t *gsRegPtr(GSRegisters &gs, uint32_t addr)
    {
        // Support both 64-bit base offsets and +4 dword aliases.
        uint32_t off = (addr - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        switch (off)
        {
        case 0x0000:
            return &gs.pmode;
        case 0x0010:
            return &gs.smode1;
        case 0x0020:
            return &gs.smode2;
        case 0x0030:
            return &gs.srfsh;
        case 0x0040:
            return &gs.synch1;
        case 0x0050:
            return &gs.synch2;
        case 0x0060:
            return &gs.syncv;
        case 0x0070:
            return &gs.dispfb1;
        case 0x0080:
            return &gs.display1;
        case 0x0090:
            return &gs.dispfb2;
        case 0x00A0:
            return &gs.display2;
        case 0x00B0:
            return &gs.extbuf;
        case 0x00C0:
            return &gs.extdata;
        case 0x00D0:
            return &gs.extwrite;
        case 0x00E0:
            return &gs.bgcolor;
        // CSR (offset 0x1000) is intentionally not handled here: it is
        // std::atomic<uint64_t> and no longer converts to uint64_t*. Callers must
        // check for offset 0x1000 themselves and go through writeCsrHalf/
        // writeCsrFull/gs.csr.load() instead of gsRegPtr().
        case 0x1010:
            return &gs.imr;
        case 0x1040:
            return &gs.busdir;
        case 0x1080:
            return &gs.siglblid;
        default:
            return nullptr;
        }
    }

    constexpr uint32_t kGsCsrRegOffset = 0x1000u;

    // [DoD] CSR.FIFO (bits 14-15) reads "empty" (01) as in PCSX2: the GIF reaches the GS synchronously here, so the
    // host FIFO never holds data. With 00 ("neither"), a game loop waiting for an empty FIFO spun until the next field
    // (Urban Reign port, PROGRESS §35). DOD_GS_CSR_FIFO=0 reads the stored bits.
    uint64_t dodCsrWithFifoEmpty(uint64_t csr)
    {
        static const bool on = [] { const char *v = std::getenv("DOD_GS_CSR_FIFO"); return !v || *v != '0'; }();
        return on ? ((csr & ~0xC000ull) | 0x4000ull) : csr;
    }

    // Atomically apply a 32-bit write to one half (off=0 low dword, off=4 high
    // dword) of the GS CSR register. Bits 0..1 of the low dword (SIGNAL/FINISH) are
    // write-one-to-clear; everything else is a plain merge. Uses compare_exchange
    // so the whole read-modify-write is a single atomic step -- this register is
    // also touched by the vsync worker (FIELD bit) and the GIF (SIGNAL/FINISH) on
    // other threads, so a load-then-store here would race with them.
    inline void writeCsrHalf(std::atomic<uint64_t> &csr, uint32_t off, uint32_t value)
    {
        constexpr uint32_t kW1cMask = 0x3u;
        uint64_t expected = csr.load();
        uint64_t desired;
        do
        {
            if (off == 0u)
            {
                uint32_t oldLow = static_cast<uint32_t>(expected & 0xFFFFFFFFull);
                uint32_t mergedLow = (oldLow & kW1cMask) | (value & ~kW1cMask);
                desired = (expected & 0xFFFFFFFF00000000ull) | static_cast<uint64_t>(mergedLow);
                desired &= ~static_cast<uint64_t>(value & kW1cMask);
            }
            else
            {
                uint64_t mask = 0xFFFFFFFFull << (off * 8u);
                desired = (expected & ~mask) | (static_cast<uint64_t>(value) << (off * 8u));
            }
        } while (!csr.compare_exchange_weak(expected, desired));
        if (off == 0u && (value & 0x200u) != 0u)
            dodpcsx2::reset(); // [DoD] CSR.RESET
    }

    // Same as writeCsrHalf but for a full 64-bit CSR write (bits 0..1 are still
    // write-one-to-clear against the current value).
    inline void writeCsrFull(std::atomic<uint64_t> &csr, uint64_t value)
    {
        constexpr uint64_t kW1cMask = 0x3ull;
        uint64_t expected = csr.load();
        uint64_t desired;
        do
        {
            desired = (expected & kW1cMask) | (value & ~kW1cMask);
            desired &= ~(value & kW1cMask);
        } while (!csr.compare_exchange_weak(expected, desired));
        if ((value & 0x200ull) != 0u)
            dodpcsx2::reset(); // [DoD] CSR.RESET
    }

    constexpr std::array<uint32_t, 4> kEeTimerBases = {
        0x10000000u,
        0x10000800u,
        0x10001000u,
        0x10001800u,
    };
    constexpr uint32_t kEeTimerCountOffset = 0x00u;
    constexpr uint32_t kEeTimerModeOffset = 0x10u;
    constexpr uint32_t kEeTimerCompareOffset = 0x20u;
    constexpr uint32_t kEeTimerHoldOffset = 0x30u;
    constexpr uint32_t kEeTimerModeClksMask = 0x3u;
    constexpr uint32_t kEeTimerModeConfigMask = 0x3FFu;
    constexpr uint32_t kEeTimerModeStatusMask = 0xC00u;
    constexpr uint32_t kEeTimerModeZret = 1u << 6;
    constexpr uint32_t kEeTimerModeCue = 1u << 7;
    constexpr uint32_t kEeTimerModeCmpe = 1u << 8;
    constexpr uint32_t kEeTimerModeOvfe = 1u << 9;
    constexpr uint32_t kEeTimerModeEquf = 1u << 10;
    constexpr uint32_t kEeTimerModeOvff = 1u << 11;
    constexpr uint64_t kEeClockHz = 294912000ull;
    constexpr std::array<uint64_t, 4> kEeTimerClockHz = {
        147456000ull,
        9216000ull,
        576000ull,
        31468ull, // [DoD] H-BLANK as PCSX2 counts it: 525 per NTSC field (Urban Reign port)
    };

    inline bool decodeEeTimerRegister(uint32_t address, size_t &timerIndex, uint32_t &offset)
    {
        for (size_t index = 0; index < kEeTimerBases.size(); ++index)
        {
            const uint32_t candidateOffset = address - kEeTimerBases[index];
            if (candidateOffset == kEeTimerCountOffset ||
                candidateOffset == kEeTimerModeOffset ||
                candidateOffset == kEeTimerCompareOffset ||
                (index < 2u && candidateOffset == kEeTimerHoldOffset))
            {
                timerIndex = index;
                offset = candidateOffset;
                return true;
            }
        }
        return false;
    }

    constexpr uint64_t ticksUntilMatch(uint32_t count, uint32_t target)
    {
        const uint32_t distance = (target - count) & 0xFFFFu;
        return distance == 0u ? 0x10000ull : static_cast<uint64_t>(distance);
    }

    // [DoD] Lazy EE timers (KillzoneRecomp 0019; PCSX2's rcntUpdate likewise only runs at the next counter event).
    // The scheduler reports cycles on every guest backward branch and call, and settling the timers each time (a
    // GIF_STAT hash lookup and four 64-bit divisions per running timer) was 8.8% of the EE thread in room 0x386
    // (RUNTIME_PATCHES row 118). The cycles now accumulate until the next timer interrupt is due; reads and writes of
    // the timer registers and of GIF_STAT, and the GIF FQC update, settle them first. A settle before the next event
    // raises no interrupt and gives the same counts (the tick fraction carries over exactly), so what the guest sees
    // is unchanged. DOD_EE_TIMER_LAZY=0 settles on every report as before.
    struct LazyEeTimers
    {
        const PS2Memory *owner = nullptr; // the memory these cycles belong to (the tests build several)
        uint64_t pendingCycles = 0;        // reported but not applied yet
        uint64_t nextEventCycles = 0;      // pending cycles at which an interrupt is due; 0 = settle on the next report
        uint32_t lateInterrupts = 0;       // raised by an early settle (not expected); returned by the next report
    };
    LazyEeTimers g_dodLazyEeTimers;

    inline bool dodLazyEeTimersEnabled()
    {
        static const bool enabled = []() {
            const char *v = std::getenv("DOD_EE_TIMER_LAZY");
            return !(v && v[0] == '0');
        }();
        return enabled;
    }

    // Applies the pending cycles now (before the guest observes or changes timer state).
    inline void dodSettleEeTimersNow(PS2Memory &memory)
    {
        LazyEeTimers &lazy = g_dodLazyEeTimers;
        if (!dodLazyEeTimersEnabled() || lazy.owner != &memory || lazy.pendingCycles == 0u)
            return;
        lazy.nextEventCycles = 0u;
        const uint32_t raised = memory.advanceEeTimers(0u);
        if (raised != 0u)
        {
            lazy.lateInterrupts |= raised;
            lazy.nextEventCycles = 0u; // hand them to the scheduler on its next report
        }
    }

    // Timer state changed: the distance to the next event must be computed again.
    inline void dodInvalidateEeTimerEvent(const PS2Memory &memory)
    {
        if (g_dodLazyEeTimers.owner == &memory)
            g_dodLazyEeTimers.nextEventCycles = 0u;
    }

    struct DmaTagView
    {
        uint16_t qwc = 0;
        uint8_t id = 0;
        bool irq = false;
        uint32_t addr = 0;
        uint32_t upper = 0;
    };

    inline DmaTagView decodeDmaTag(uint64_t tag)
    {
        DmaTagView out{};
        out.qwc = static_cast<uint16_t>(tag & 0xFFFFu);
        out.id = static_cast<uint8_t>((tag >> 28u) & 0x7u);
        out.irq = ((tag >> 31u) & 0x1ull) != 0ull;
        out.addr = static_cast<uint32_t>((tag >> 32u) & 0x7FFFFFFFu);
        out.upper = static_cast<uint32_t>((tag >> 16u) & 0xFFFFu);
        return out;
    }

    inline uint32_t gifTagNloop(uint64_t tagLo)
    {
        return static_cast<uint32_t>(tagLo & 0x7FFFu);
    }

    inline uint8_t gifTagFlg(uint64_t tagLo)
    {
        return static_cast<uint8_t>((tagLo >> 58u) & 0x3u);
    }

    inline uint32_t gifTagNreg(uint64_t tagLo)
    {
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60u) & 0xFu);
        return nreg == 0u ? 16u : nreg;
    }

}

// Helpers for GS VRAM addressing (PSMCT32 path).
static inline uint32_t gs_vram_offset(uint32_t basePage, uint32_t x, uint32_t y, uint32_t fbw)
{
    // basePage is in 2048-byte units; fbw is in blocks of 64 pixels.
    uint32_t strideBytes = fbw * 64 * 4;
    return basePage * 2048 + y * strideBytes + x * 4;
}

PS2Memory::PS2Memory()
    : m_rdram(nullptr), m_scratchpad(nullptr), iop_ram(nullptr), m_seenGifCopy(false), m_gsVRAM(nullptr)
{
    ps2SetScratchpadHostPtr(nullptr);
}

PS2Memory::~PS2Memory()
{
    if (m_rdram)
    {
        delete[] m_rdram;
        m_rdram = nullptr;
    }

    if (m_scratchpad)
    {
        ps2SetScratchpadHostPtr(nullptr);
        delete[] m_scratchpad;
        m_scratchpad = nullptr;
    }

    if (m_gsVRAM)
    {
        delete[] m_gsVRAM;
        m_gsVRAM = nullptr;
    }

    if (m_vu1Code)
    {
        delete[] m_vu1Code;
        m_vu1Code = nullptr;
    }
    if (m_vu1Data)
    {
        delete[] m_vu1Data;
        m_vu1Data = nullptr;
    }
    if (m_vu0Code)
    {
        delete[] m_vu0Code;
        m_vu0Code = nullptr;
    }
    if (m_vu0Data)
    {
        delete[] m_vu0Data;
        m_vu0Data = nullptr;
    }

    if (iop_ram)
    {
        delete[] iop_ram;
        iop_ram = nullptr;
    }
}

bool PS2Memory::initialize(size_t ramSize)
{
    auto cleanup = [this]()
    {
        delete[] m_rdram;
        delete[] m_scratchpad;
        delete[] iop_ram;
        delete[] m_gsVRAM;
        delete[] m_vu0Code;
        delete[] m_vu0Data;
        delete[] m_vu1Code;
        delete[] m_vu1Data;
        m_rdram = nullptr;
        m_scratchpad = nullptr;
        ps2SetScratchpadHostPtr(nullptr);
        iop_ram = nullptr;
        m_gsVRAM = nullptr;
        m_vu0Code = nullptr;
        m_vu0Data = nullptr;
        m_vu1Code = nullptr;
        m_vu1Data = nullptr;
    };

    cleanup();
    m_seenGifCopy = false;
    m_dmaStartCount.store(0, std::memory_order_relaxed);
    m_gifCopyCount.store(0, std::memory_order_relaxed);
    m_gsWriteCount.store(0, std::memory_order_relaxed);
    m_vifWriteCount.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(m_completedDmacMutex);
        m_completedDmacCauses.clear();
    }
    m_codeRegions.clear();
    m_path3Masked = false;
    m_path3MaskedFifo.clear();
    m_vif1PendingPath2ImageQwc = 0u;
    m_vif1PendingPath2DirectHl = false;
    g_dodPath3PendingImageQwc = 0u; // [DoD] process-wide, reset with the GIF (see GIF_CTRL.RST in writeIORegister)
    resetEeTimers();

    try
    {
        // Allocate main RAM
        m_rdram = new uint8_t[ramSize];
        std::memset(m_rdram, 0, ramSize);

        // Allocate scratchpad
        m_scratchpad = new uint8_t[PS2_SCRATCHPAD_SIZE];
        std::memset(m_scratchpad, 0, PS2_SCRATCHPAD_SIZE);
        ps2SetScratchpadHostPtr(m_scratchpad);

        // Initialize EE TLB entries (R5900 has 48 entries).
        m_tlbEntries.assign(48, TLBEntry{0, 0, 0, false});

        // Allocate IOP RAM
        iop_ram = new uint8_t[2 * 1024 * 1024]; // 2MB

        // Initialize IOP RAM with zeros
        std::memset(iop_ram, 0, 2 * 1024 * 1024);

        // Initialize I/O registers
        m_ioRegisters.clear();
        g_dodMpegToIpu.abort();
        dod_mpeg_dma::endAll();

        // Initialize GS registers
        memset(&gs_regs, 0, sizeof(gs_regs));
        // memset zero-fills std::atomic<uint64_t>::csr's bytes, which is not itself
        // a guaranteed-valid atomic store; make the zero-initialization explicit.
        gs_regs.csr.store(0);
        gs_regs.dispfb1 = (0ULL << 0) | (10ULL << 9) | (0ULL << 15) | (0ULL << 32) | (0ULL << 43);
        gs_regs.display1 = (0ULL << 0) | (0ULL << 12) | (0ULL << 23) | (0ULL << 27) | (639ULL << 32) | (447ULL << 44);
        gs_regs.dispfb2 = gs_regs.dispfb1;
        gs_regs.display2 = gs_regs.display1;

        // Allocate GS VRAM (4MB)
        m_gsVRAM = new uint8_t[PS2_GS_VRAM_SIZE];
        std::memset(m_gsVRAM, 0, PS2_GS_VRAM_SIZE);

        m_vu0Code = new uint8_t[PS2_VU0_CODE_SIZE];
        m_vu0Data = new uint8_t[PS2_VU0_DATA_SIZE];
        std::memset(m_vu0Code, 0, PS2_VU0_CODE_SIZE);
        std::memset(m_vu0Data, 0, PS2_VU0_DATA_SIZE);

        m_vu1Code = new uint8_t[PS2_VU1_CODE_SIZE];
        m_vu1Data = new uint8_t[PS2_VU1_DATA_SIZE];
        std::memset(m_vu1Code, 0, PS2_VU1_CODE_SIZE);
        std::memset(m_vu1Data, 0, PS2_VU1_DATA_SIZE);
        markVU0CodeModified();
        markVU1CodeModified();

        // Initialize VIF registers
        memset(&vif0_regs, 0, sizeof(vif0_regs));
        memset(&vif1_regs, 0, sizeof(vif1_regs));

        // Initialize DMA registers
        memset(dma_regs, 0, sizeof(dma_regs));

        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error initializing PS2 memory: " << e.what() << std::endl;
        cleanup();
        return false;
    }
}

void PS2Memory::resetEeTimers() noexcept
{
    m_eeTimers = {};
    // [DoD] The BIOS leaves T3 counting H-BLANKs for its alarms, and games read T3_COUNT for timing; the runtime
    // emulates alarms natively and never started it (Urban Reign port, PROGRESS "Bugs do runtime" 3).
    m_eeTimers[3].mode = kEeTimerModeCue | 3u;
    g_dodLazyEeTimers = LazyEeTimers{};
    g_dodLazyEeTimers.owner = this;
}

uint32_t PS2Memory::advanceEeTimers(uint64_t eeCycles) noexcept
{
    const bool lazy = dodLazyEeTimersEnabled();
    if (lazy)
    {
        LazyEeTimers &state = g_dodLazyEeTimers;
        if (state.owner != this)
        {
            state = LazyEeTimers{};
            state.owner = this;
        }
        state.pendingCycles += eeCycles;
        if (state.pendingCycles < state.nextEventCycles || state.pendingCycles == 0u)
        {
            return 0u;
        }
        eeCycles = state.pendingCycles;
        state.pendingCycles = 0u;
    }
    if (eeCycles == 0u)
    {
        return 0u;
    }

    constexpr uint32_t kGifStat = 0x10003020u;
    constexpr uint32_t kGifFqcMask = 0x1F000000u;
    auto gifStatIt = m_ioRegisters.find(kGifStat);
    if (gifStatIt != m_ioRegisters.end())
        gifStatIt->second &= ~kGifFqcMask;

    uint32_t interruptMask = 0u;
    for (size_t index = 0; index < m_eeTimers.size(); ++index)
    {
        EeTimer &timer = m_eeTimers[index];
        if ((timer.mode & kEeTimerModeCue) == 0u)
        {
            continue;
        }

        const uint64_t clockHz = kEeTimerClockHz[timer.mode & kEeTimerModeClksMask];
        const uint64_t wholeSeconds = eeCycles / kEeClockHz;
        const uint64_t remainingCycles = eeCycles % kEeClockHz;
        const uint64_t scaled = remainingCycles * clockHz + timer.clockRemainder;
        const uint64_t ticks = wholeSeconds * clockHz + scaled / kEeClockHz;
        timer.clockRemainder = scaled % kEeClockHz;
        if (ticks == 0u)
        {
            continue;
        }

        const uint32_t oldCount = timer.count & 0xFFFFu;
        const uint32_t compare = timer.compare & 0xFFFFu;
        const uint64_t compareDistance = ticksUntilMatch(oldCount, compare);
        const uint64_t overflowDistance = 0x10000ull - oldCount;
        const bool zeroReturn = (timer.mode & kEeTimerModeZret) != 0u;
        const bool compareReached = ticks >= compareDistance;
        bool overflowReached = false;

        if (zeroReturn)
        {
            overflowReached = ticks >= overflowDistance && overflowDistance <= compareDistance;
            if (compareReached)
            {
                const uint64_t remaining = ticks - compareDistance;
                timer.count = compare == 0u
                                  ? static_cast<uint32_t>(remaining & 0xFFFFu)
                                  : static_cast<uint32_t>(remaining % compare);
            }
            else
            {
                timer.count = static_cast<uint32_t>((oldCount + ticks) & 0xFFFFu);
            }
        }
        else
        {
            overflowReached = ticks >= overflowDistance;
            timer.count = static_cast<uint32_t>((oldCount + ticks) & 0xFFFFu);
        }

        if (compareReached && (timer.mode & kEeTimerModeCmpe) != 0u && (timer.mode & kEeTimerModeEquf) == 0u)
        {
            timer.mode |= kEeTimerModeEquf;
            interruptMask |= 1u << index;
        }
        if (overflowReached && (timer.mode & kEeTimerModeOvfe) != 0u && (timer.mode & kEeTimerModeOvff) == 0u)
        {
            timer.mode |= kEeTimerModeOvff;
            interruptMask |= 1u << index;
        }
    }
    if (lazy)
    {
        interruptMask |= g_dodLazyEeTimers.lateInterrupts;
        g_dodLazyEeTimers.lateInterrupts = 0u;
        g_dodLazyEeTimers.nextEventCycles = cyclesUntilNextEeTimerInterrupt();
    }
    return interruptMask;
}

uint64_t PS2Memory::cyclesUntilNextEeTimerInterrupt() const noexcept
{
    uint64_t nearest = std::numeric_limits<uint64_t>::max();
    for (const EeTimer &timer : m_eeTimers)
    {
        if ((timer.mode & kEeTimerModeCue) == 0u)
        {
            continue;
        }

        const uint32_t count = timer.count & 0xFFFFu;
        const uint32_t compare = timer.compare & 0xFFFFu;
        const uint64_t compareDistance = ticksUntilMatch(count, compare);
        const uint64_t overflowDistance = 0x10000ull - count;
        uint64_t eventTicks = std::numeric_limits<uint64_t>::max();

        if ((timer.mode & kEeTimerModeCmpe) != 0u &&
            (timer.mode & kEeTimerModeEquf) == 0u)
        {
            eventTicks = compareDistance;
        }
        const bool overflowCanOccur = (timer.mode & kEeTimerModeZret) == 0u ||
                                      overflowDistance <= compareDistance;
        if (overflowCanOccur &&
            (timer.mode & kEeTimerModeOvfe) != 0u &&
            (timer.mode & kEeTimerModeOvff) == 0u)
        {
            eventTicks = std::min(eventTicks, overflowDistance);
        }
        if (eventTicks == std::numeric_limits<uint64_t>::max())
        {
            continue;
        }

        const uint64_t clockHz = kEeTimerClockHz[timer.mode & kEeTimerModeClksMask];
        const uint64_t numerator = eventTicks * kEeClockHz - timer.clockRemainder;
        const uint64_t cycles = (numerator + clockHz - 1u) / clockHz;
        nearest = std::min(nearest, std::max<uint64_t>(1u, cycles));
    }
    // [DoD] Lazy timers: the state above is as of the last settle; the pending cycles have already run.
    if (dodLazyEeTimersEnabled() && g_dodLazyEeTimers.owner == this &&
        nearest != std::numeric_limits<uint64_t>::max())
    {
        const uint64_t pending = g_dodLazyEeTimers.pendingCycles;
        nearest = nearest > pending ? nearest - pending : 1u;
    }
    return nearest;
}

bool PS2Memory::isScratchpad(uint32_t address) const
{
    return ps2IsScratchpadAddress(address);
}

uint8_t *PS2Memory::mapVuMemory(uint32_t physAddr, uint32_t size, uint32_t &offset, uint32_t &limit)
{
    return const_cast<uint8_t *>(static_cast<const PS2Memory *>(this)->mapVuMemory(physAddr, size, offset, limit));
}

const uint8_t *PS2Memory::mapVuMemory(uint32_t physAddr, uint32_t size, uint32_t &offset, uint32_t &limit) const
{
    auto mapRange = [&](uint32_t base, uint32_t rangeSize, const uint8_t *ptr) -> const uint8_t *
    {
        if (!ptr || physAddr < base)
        {
            return nullptr;
        }
        const uint32_t local = physAddr - base;
        if (local >= rangeSize || size > (rangeSize - local))
        {
            return nullptr;
        }
        offset = local;
        limit = rangeSize;
        return ptr;
    };

    if (const uint8_t *ptr = mapRange(PS2_VU0_CODE_BASE, PS2_VU0_CODE_SIZE, m_vu0Code))
    {
        return ptr;
    }
    if (const uint8_t *ptr = mapRange(PS2_VU0_DATA_BASE, PS2_VU0_DATA_SIZE, m_vu0Data))
    {
        return ptr;
    }
    if (const uint8_t *ptr = mapRange(PS2_VU1_CODE_BASE, PS2_VU1_CODE_SIZE, m_vu1Code))
    {
        return ptr;
    }
    return mapRange(PS2_VU1_DATA_BASE, PS2_VU1_DATA_SIZE, m_vu1Data);
}

uint32_t PS2Memory::translateAddress(uint32_t virtualAddress)
{
    if (isScratchpad(virtualAddress))
    {
        return ps2ScratchpadOffset(virtualAddress);
    }

    // EE uncached aliases of main RAM (per PS2 memory map):
    //   0x20000000-0x3FFFFFFF -> 32MB mirror of RDRAM
    // This includes the accelerated window rooted at 0x30100000.
    if (Ps2IsUncachedRamMirrorAddress(virtualAddress))
    {
        return virtualAddress & PS2_RAM_MASK;
    }

    // KSEG0/KSEG1 direct-mapped window.
    if (Ps2IsKseg01Address(virtualAddress))
    {
        return Ps2DirectMappedPhysicalAddress(virtualAddress);
    }

    // In this runtime, low segments are treated as physical-style addresses already.
    if (virtualAddress < 0x80000000)
    {
        return virtualAddress;
    }

    // KSEG2/KSEG3 are TLB mapped.
    if (Ps2IsKseg23Address(virtualAddress))
    {
        for (const auto &entry : m_tlbEntries)
        {
            if (entry.valid)
            {
                // PageMask uses bits [24:13]. Build an address-level mask (plus 4KB base page bits).
                const uint32_t mask = entry.mask & 0x01FFE000u;
                const uint32_t compareMask = ~(mask | 0xFFFu);
                if ((virtualAddress & compareMask) == (entry.vpn & compareMask))
                {
                    // TLB hit
                    const uint32_t pageOffsetMask = mask | 0xFFFu;
                    const uint32_t physBase = entry.pfn << 12;
                    return physBase | (virtualAddress & pageOffsetMask);
                }
            }
        }
        throw std::runtime_error("TLB miss for address: 0x" + std::to_string(virtualAddress));
    }

    return virtualAddress;
}

bool PS2Memory::tlbRead(uint32_t index, uint32_t &vpn, uint32_t &pfn, uint32_t &mask, bool &valid) const
{
    if (index >= m_tlbEntries.size())
    {
        return false;
    }

    const TLBEntry &entry = m_tlbEntries[index];
    vpn = entry.vpn;
    pfn = entry.pfn;
    mask = entry.mask;
    valid = entry.valid;
    return true;
}

bool PS2Memory::tlbWrite(uint32_t index, uint32_t vpn, uint32_t pfn, uint32_t mask, bool valid)
{
    if (index >= m_tlbEntries.size())
    {
        return false;
    }

    TLBEntry &entry = m_tlbEntries[index];
    entry.vpn = vpn & 0xFFFFF000u;
    entry.pfn = pfn & 0x000FFFFFu;
    entry.mask = mask & 0x01FFE000u;
    entry.valid = valid;
    return true;
}

int32_t PS2Memory::tlbProbe(uint32_t vpn) const
{
    const uint32_t normalizedVpn = vpn & 0xFFFFF000u;
    for (uint32_t i = 0; i < static_cast<uint32_t>(m_tlbEntries.size()); ++i)
    {
        const TLBEntry &entry = m_tlbEntries[i];
        if (!entry.valid)
        {
            continue;
        }

        const uint32_t mask = entry.mask & 0x01FFE000u;
        const uint32_t compareMask = ~(mask | 0xFFFu);
        if ((normalizedVpn & compareMask) == (entry.vpn & compareMask))
        {
            return static_cast<int32_t>(i);
        }
    }

    return -1;
}

uint8_t PS2Memory::read8(uint32_t address)
{
    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return m_scratchpad[physAddr];
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return m_rdram[physAddr];
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint8_t), vuOffset, vuLimit))
    {
        (void)vuLimit;
        return vuMem[vuOffset];
    }
    else if (isIoRegister(physAddr))
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t value = readIORegister(regAddr);
        uint32_t shift = (physAddr & 3) * 8;
        return static_cast<uint8_t>((value >> shift) & 0xFF);
    }

    return 0;
}

uint16_t PS2Memory::read16(uint32_t address)
{
    if (address & 1)
    {
        throw std::runtime_error("Unaligned 16-bit read at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return loadScalar<uint16_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read16 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint16_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read16 rdram", address);
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint16_t), vuOffset, vuLimit))
    {
        return loadScalar<uint16_t>(vuMem, vuOffset, vuLimit, "read16 vu", address);
    }
    else if (isIoRegister(physAddr))
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t value = readIORegister(regAddr);
        uint32_t shift = (physAddr & 2) * 8;
        return static_cast<uint16_t>((value >> shift) & 0xFFFF);
    }

    return 0;
}

uint32_t PS2Memory::read32(uint32_t address)
{
    if (address & 3)
    {
        throw std::runtime_error("Unaligned 32-bit read at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        dodio::note(address, false, 0u); // [DoD] DOD_IO_CENSUS
        uint32_t off = address & 7;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            uint64_t val = dodCsrWithFifoEmpty(gs_regs.csr.load());
            return (uint32_t)(val >> (off * 8));
        }
        uint64_t *reg = gsRegPtr(gs_regs, address);
        if (!reg)
            return 0;
        uint64_t val = *reg;
        return (uint32_t)(val >> (off * 8));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return loadScalar<uint32_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read32 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint32_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read32 rdram", address);
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint32_t), vuOffset, vuLimit))
    {
        return loadScalar<uint32_t>(vuMem, vuOffset, vuLimit, "read32 vu", address);
    }
    else if (isIoRegister(physAddr))
    {
        return readIORegister(physAddr);
    }

    return 0;
}

uint64_t PS2Memory::read64(uint32_t address)
{
    if (address & 7)
    {
        throw std::runtime_error("Unaligned 64-bit read at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        dodio::note(address, false, 0u); // [DoD] DOD_IO_CENSUS
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            return dodCsrWithFifoEmpty(gs_regs.csr.load());
        }
        uint64_t *reg = gsRegPtr(gs_regs, address);
        return reg ? *reg : 0;
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return loadScalar<uint64_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read64 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint64_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read64 rdram", address);
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint64_t), vuOffset, vuLimit))
    {
        return loadScalar<uint64_t>(vuMem, vuOffset, vuLimit, "read64 vu", address);
    }

    if (physAddr >= 0x10002000u && physAddr < 0x10002040u && dodac5ipu::enabled())
    {
        dodac5ipu::attach(*this);
        const uint64_t value = static_cast<uint64_t>(ps2_ipu_read32(physAddr)) | (static_cast<uint64_t>(ps2_ipu_read32(physAddr + 4u)) << 32);
        {
            // [DoD] DOD_IPU_BUSYLOG=<n>: the first n 64-bit IPU register reads that return BUSY.
            static long busyLog = [] { const char *v = std::getenv("DOD_IPU_BUSYLOG"); return v ? std::atol(v) : 0L; }();
            if (busyLog > 0 && (value >> 63) != 0u)
            {
                --busyLog;
                std::fprintf(stderr, "[ipu-busy] read64 %08X = %016llX ctrl=%08X bp=%08X pc=%08X ra=%08X\n", physAddr,
                             static_cast<unsigned long long>(value), ps2_ipu_read32(0x10002010u), ps2_ipu_read32(0x10002020u),
                             dodLastGuestPc(), dodLastGuestRa());
            }
        }
        return value;
    }
    if (physAddr >= 0x10002000u && physAddr < 0x10002040u) // [DoD] IPU_CMD / IPU_TOP carry BUSY in the upper word
    {
        const uint32_t off = physAddr - 0x10002000u;
        return static_cast<uint64_t>(ps2_ipu::read32(off)) | (static_cast<uint64_t>(ps2_ipu::read32(off + 4u)) << 32);
    }

    // 64-bit IO read: compose from the two adjacent 32-bit IO register slots
    // to avoid any side-effects from read32 handlers.
    if (isIoRegister(address))
    {
        uint32_t lo = m_ioRegisters.count(address) ? m_ioRegisters[address] : 0u;
        uint32_t hi = m_ioRegisters.count(address + 4) ? m_ioRegisters[address + 4] : 0u;
        dodio::note(address, false, static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32)); // [DoD] DOD_IO_CENSUS
        return static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
    }
    return (uint64_t)read32(address) | ((uint64_t)read32(address + 4) << 32);
}

__m128i PS2Memory::read128(uint32_t address)
{
    if (address & 15)
    {
        throw std::runtime_error("Unaligned 128-bit read at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        inRange(physAddr, sizeof(__m128i), PS2_SCRATCHPAD_SIZE, "read128 scratchpad", address);
        return _mm_loadu_si128(reinterpret_cast<__m128i *>(&m_scratchpad[physAddr]));
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        inRange(physAddr, sizeof(__m128i), PS2_RAM_SIZE, "read128 rdram", address);
        return _mm_loadu_si128(reinterpret_cast<__m128i *>(&m_rdram[physAddr]));
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(__m128i), vuOffset, vuLimit))
    {
        inRange(vuOffset, sizeof(__m128i), vuLimit, "read128 vu", address);
        return _mm_loadu_si128(reinterpret_cast<const __m128i *>(vuMem + vuOffset));
    }

    if (!scratch && physAddr == 0x10007000u) // [DoD] IPU_out_FIFO
    {
        dodio::note(physAddr, false, 0u); // [DoD] DOD_IO_CENSUS
        alignas(16) uint8_t fifoData[16] = {};
        if (dodac5ipu::enabled())
        {
            dodac5ipu::attach(*this);
            ps2_ipu_dma_out(fifoData, 1u);
            return _mm_load_si128(reinterpret_cast<const __m128i *>(fifoData));
        }
        ps2_ipu::popOutput(fifoData, sizeof(fifoData));
        return _mm_load_si128(reinterpret_cast<const __m128i *>(fifoData));
    }

    // 128-bit reads are primarily for quad-word loads in the EE, which are only valid for RAM areas
    // Return zeroes for unsupported areas
    return _mm_setzero_si128();
}

void PS2Memory::write8(uint32_t address, uint8_t value)
{
    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        m_scratchpad[physAddr] = value;
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        m_rdram[physAddr] = value;
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint8_t), vuOffset, vuLimit))
        {
            (void)vuLimit;
            vuMem[vuOffset] = value;
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        // IO registers - handle byte writes by modifying the appropriate byte in the word
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t shift = (physAddr & 3) * 8;
        uint32_t mask = ~(0xFF << shift);
        uint32_t newValue = (m_ioRegisters[regAddr] & mask) | ((uint32_t)value << shift);
        writeIORegister(regAddr, newValue);
    }
}

void PS2Memory::write16(uint32_t address, uint16_t value)
{
    if (address & 1)
    {
        throw std::runtime_error("Unaligned 16-bit write at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        storeScalar<uint16_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write16 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        storeScalar<uint16_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write16 rdram", address);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint16_t), vuOffset, vuLimit))
        {
            storeScalar<uint16_t>(vuMem, vuOffset, vuLimit, value, "write16 vu", address);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t shift = (physAddr & 2) * 8;
        uint32_t mask = ~(0xFFFF << shift);
        uint32_t newValue = (m_ioRegisters[regAddr] & mask) | ((uint32_t)value << shift);
        writeIORegister(regAddr, newValue);
    }
}

void PS2Memory::write32(uint32_t address, uint32_t value)
{
    if (address & 3)
    {
        throw std::runtime_error("Unaligned 32-bit write at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        dodio::note(address, true, value); // [DoD] DOD_IO_CENSUS
        uint32_t off = address & 7;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            // CSR: bits 0..1 of the low dword are write-one-to-clear status bits.
            // Done as a single atomic RMW -- see writeCsrHalf's comment.
            writeCsrHalf(gs_regs.csr, off, value);
        }
        else if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            uint64_t mask = 0xFFFFFFFFULL << (off * 8);
            uint64_t newVal = (*reg & ~mask) | ((uint64_t)value << (off * 8));
            *reg = newVal;
#if defined(DOD_FUNCTION_RING)
            if ((regOff == 0x70u || regOff == 0x90u || regOff == 0xE0u) && (value & 0xFFF00000u) != 0u)
            {
                static int s_logged32 = 0; // [DoD] diagnostics: 32-bit junk writes to DISPFB/BGCOLOR
                if (s_logged32 < 8)
                {
                    ++s_logged32;
                    std::string last;
                    for (uint32_t i = dod_ring::g_next - 6u; i != dod_ring::g_next; ++i)
                    {
                        const char *n = dod_ring::g_names[i & (dod_ring::kSize - 1u)];
                        last += n ? n : "?";
                        last += ' ';
                    }
                    std::fprintf(stderr, "[gs priv] write32 0x%08X = %08X after: %s\n", address, value, last.c_str());
                }
            }
#endif
        }
        return;
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        storeScalar<uint32_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write32 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        // Check if this might be code modification
        markModified(address, 4);

        storeScalar<uint32_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write32 rdram", address);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint32_t), vuOffset, vuLimit))
        {
            storeScalar<uint32_t>(vuMem, vuOffset, vuLimit, value, "write32 vu", address);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        writeIORegister(physAddr, value);
    }
}

void PS2Memory::write64(uint32_t address, uint64_t value)
{
    if (address & 7)
    {
        throw std::runtime_error("Unaligned 64-bit write at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        dodio::note(address, true, value); // [DoD] DOD_IO_CENSUS
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
#if defined(DOD_FUNCTION_RING)
        const bool bogusDispfb = (regOff == 0x70u || regOff == 0x90u) && (value & 0xFFC00000FFF00000ull) != 0u;
        const bool bogusBgcolor = regOff == 0xE0u && (value & ~0xFFFFFFull) != 0u;
        if (bogusDispfb || bogusBgcolor)
        {
            static int s_logged = 0; // [DoD] diagnostics: who writes junk into DISPFB1/DISPFB2/BGCOLOR
            if (s_logged < 12)
            {
                ++s_logged;
                std::string last;
                for (uint32_t i = dod_ring::g_next - 6u; i != dod_ring::g_next; ++i)
                {
                    const char *n = dod_ring::g_names[i & (dod_ring::kSize - 1u)];
                    last += n ? n : "?";
                    last += ' ';
                }
                std::fprintf(stderr, "[gs priv] write64 0x%08X = %016llX after: %s\n", address,
                             static_cast<unsigned long long>(value), last.c_str());
            }
        }
#endif
        if (regOff == kGsCsrRegOffset)
        {
            // CSR: bits 0..1 are write-one-to-clear status bits. Done as a single
            // atomic RMW -- see writeCsrFull's comment.
            writeCsrFull(gs_regs.csr, value);
        }
        else if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            *reg = value;
        }
        return;
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        storeScalar<uint64_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write64 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        markModified(address, 8);
        storeScalar<uint64_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write64 rdram", address);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint64_t), vuOffset, vuLimit))
        {
            storeScalar<uint64_t>(vuMem, vuOffset, vuLimit, value, "write64 vu", address);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        write32(address, (uint32_t)value);
        write32(address + 4, (uint32_t)(value >> 32));
    }
}

void PS2Memory::write128(uint32_t address, __m128i value)
{
    if (address & 15)
    {
        throw std::runtime_error("Unaligned 128-bit write at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (!scratch && physAddr >= 0x10004000u && physAddr < 0x10008000u) // [DoD] DOD_IO_CENSUS: FIFO writes
        dodio::note(physAddr, true, static_cast<uint64_t>(_mm_cvtsi128_si64(value)));
    if (!scratch && physAddr == 0x10004000u) // VIF0_FIFO
    {
        alignas(16) uint8_t fifoData[16];
        _mm_storeu_si128(reinterpret_cast<__m128i *>(fifoData), value);
        processVIF0Data(fifoData, sizeof(fifoData));
        return;
    }
    if (!scratch && physAddr == 0x10005000u) // VIF1_FIFO
    {
        alignas(16) uint8_t fifoData[16];
        _mm_storeu_si128(reinterpret_cast<__m128i *>(fifoData), value);
        processVIF1Data(fifoData, sizeof(fifoData));
        return;
    }
    if (!scratch && physAddr == 0x10007010u) // [DoD] IPU_in_FIFO
    {
        alignas(16) uint8_t fifoData[16];
        _mm_storeu_si128(reinterpret_cast<__m128i *>(fifoData), value);
        if (dodac5ipu::enabled())
        {
            dodac5ipu::attach(*this);
            static long fifoTrace = [] { const char *v = std::getenv("DOD_IPU_TRACE"); return v ? std::atol(v) : 0L; }();
            if (fifoTrace > 0)
            {
                --fifoTrace;
                std::fprintf(stderr, "[ipu-trace] in-fifo %02X %02X %02X %02X ctrl=%08X\n", fifoData[0], fifoData[1], fifoData[2],
                             fifoData[3], ps2_ipu_read32(0x10002010u));
            }
            ps2_ipu_dma_in(fifoData, 1u);
            return;
        }
        ps2_ipu::pushInput(fifoData, sizeof(fifoData));
        if (dodServiceIpuFrom(m_ioRegisters, m_rdram, m_scratchpad))
            completeDmacChannel(0x1000B000u, 3u);
        return;
    }

    if (scratch)
    {
        inRange(physAddr, sizeof(__m128i), PS2_SCRATCHPAD_SIZE, "write128 scratchpad", address);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(&m_scratchpad[physAddr]), value);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        markModified(address, 16);
        inRange(physAddr, sizeof(__m128i), PS2_RAM_SIZE, "write128 rdram", address);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(&m_rdram[physAddr]), value);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(__m128i), vuOffset, vuLimit))
        {
            inRange(vuOffset, sizeof(__m128i), vuLimit, "write128 vu", address);
            _mm_storeu_si128(reinterpret_cast<__m128i *>(vuMem + vuOffset), value);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        // Non-RAM 128-bit stores are modeled as two 64-bit stores.
        uint64_t lo = _mm_extract_epi64(value, 0);
        uint64_t hi = _mm_extract_epi64(value, 1);

        write64(address, lo);
        write64(address + 8, hi);
    }
}

// [DoD] INTC_STAT (0x1000F000) is a latch: the interrupt sources set its bits, writing 1 clears them (EE User's
// Manual). It was plain storage, so libkernel's VSync wait read back the 4 it had just written, finished at once and
// the next vblank wrote the VSync tick into a reused stack frame (Urban Reign port, PROGRESS "Bugs do runtime" 2).
void dodEeClearLatchedDmac(uint32_t channels); // Kernel/EeScheduler.cpp

void dodRaiseIntcStat(PS2Memory &memory, uint32_t bits)
{
    memory.m_ioRegisters[0x1000F000u] |= bits;
}

bool PS2Memory::writeIORegister(uint32_t address, uint32_t value)
{
    dodio::note(address, true, value); // [DoD] DOD_IO_CENSUS
    // [DoD] DOD_IPU_TRACE=<n>: the first n writes to the IPU registers and the IPU DMA channels (with the IPU state).
    if ((address >= 0x10002000u && address < 0x10002040u) || (address >= 0x1000B000u && address < 0x1000B060u) ||
        (address >= 0x1000B400u && address < 0x1000B460u))
    {
        static long left = [] { const char *v = std::getenv("DOD_IPU_TRACE"); return v ? std::atol(v) : 0L; }();
        // DOD_IPU_TRACE_MOVIE=1: start at the first BDEC/VDEC/CSC command or a toIPU start that is not the logo's.
        static bool armed = std::getenv("DOD_IPU_TRACE_MOVIE") == nullptr;
        if (!armed && ((address == 0x10002000u && ((value >> 28) == 2u || (value >> 28) == 3u || (value >> 28) == 7u)) ||
                       (address == 0x1000B400u && (value & 0x100u) != 0u && value != 0x185u)))
            armed = true;
        // DOD_IPU_PCLOG=<n>: the guest pc/ra of the first n toIPU CHCR writes that clear STR.
        static long pcLog = [] { const char *v = std::getenv("DOD_IPU_PCLOG"); return v ? std::atol(v) : 0L; }();
        if (pcLog > 0 && address == 0x1000B400u && (value & 0x104u) == 0x004u)
        {
            --pcLog;
            std::fprintf(stderr, "[ipu-pc] toIPU CHCR=%08X pc=%08X ra=%08X\n", value, dodLastGuestPc(), dodLastGuestRa());
        }
        if (left > 0 && armed)
        {
            --left;
            std::fprintf(stderr, "[ipu-trace] w %08X = %08X ctrl=%08X bp=%08X in_space=%u out=%u\n", address, value,
                         ps2_ipu_read32(0x10002010u), ps2_ipu_read32(0x10002020u), ps2_ipu_in_space(), ps2_ipu_out_avail());
        }
    }
    if (address == 0x1000F000u)
    {
        m_ioRegisters[address] &= ~value;
        return true;
    }
    // [DoD] GIF_CTRL.RST resets the GIF's path state (PCSX2 Gif_Unit::Reset): forget an IMAGE still owed by the
    // last PATH3 tag, or the next PATH3 transfer would start as its pixels.
    if (address == 0x10003000u && (value & 1u) != 0u)
        g_dodPath3PendingImageQwc = 0u;
    size_t timerIndex = 0u;
    uint32_t timerOffset = 0u;
    if (decodeEeTimerRegister(address, timerIndex, timerOffset))
    {
        dodSettleEeTimersNow(*this); // the elapsed cycles ran under the old settings
        dodInvalidateEeTimerEvent(*this);
        EeTimer &timer = m_eeTimers[timerIndex];
        switch (timerOffset)
        {
        case kEeTimerCountOffset:
            timer.count = value & 0xFFFFu;
            timer.clockRemainder = 0u;
            break;
        case kEeTimerModeOffset:
        {
            const uint32_t previousMode = timer.mode;
            const uint32_t status = (previousMode & kEeTimerModeStatusMask) & ~(value & kEeTimerModeStatusMask);
            timer.mode = (value & kEeTimerModeConfigMask) | status;
            if (((previousMode ^ timer.mode) & (kEeTimerModeClksMask | kEeTimerModeCue)) != 0u)
            {
                timer.clockRemainder = 0u;
            }
            break;
        }
        case kEeTimerCompareOffset:
            timer.compare = value & 0xFFFFu;
            break;
        case kEeTimerHoldOffset:
            timer.hold = value & 0xFFFFu;
            break;
        default:
            return false;
        }
        return true;
    }

    if (isGsPrivReg(address))
    {
        // NB: unreachable from write8/16/32/64 today since those all funnel IO
        // register writes through addresses in PS2_IO_BASE's range, which is
        // disjoint from PS2_GS_PRIV_REG_BASE; kept correct for direct callers.
        m_ioRegisters[address] = value;
        const uint32_t off = address & 7u;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            writeCsrHalf(gs_regs.csr, off, value);
        }
        else if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            const uint64_t mask = 0xFFFFFFFFull << (off * 8u);
            *reg = (*reg & ~mask) | (static_cast<uint64_t>(value) << (off * 8u));
        }
        m_gsWriteCount.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    if (address >= 0x10002000 && address < 0x10002040 && dodac5ipu::enabled())
    {
        ++g_dodIpuRegisterWrites;
        dodac5ipu::attach(*this);
        ps2_ipu_write32(address, value);
        return true;
    }
    if (address >= 0x10002000 && address < 0x10002040)
    {
        // [DoD] IPU registers (ps2_ipu.cpp)
        ++g_dodIpuRegisterWrites;
        ps2_ipu::write32(address - 0x10002000u, value);
        if (dodServiceIpuFrom(m_ioRegisters, m_rdram, m_scratchpad))
            completeDmacChannel(0x1000B000u, 3u);
        return true;
    }

    if (address == 0x1000E010u)
    {
        const uint32_t current = m_ioRegisters.count(address) ? m_ioRegisters[address] : 0u;
        uint32_t status = current & 0x3FFu;
        uint32_t mask = (current >> 16) & 0x3FFu;

        // D_STAT low bits are W1C status, high bits [16..25] toggle masks on write-one.
        status &= ~(value & 0x3FFu);
        if ((value & 0x3FFu) != 0u)
            dodEeClearLatchedDmac(value & 0x3FFu); // [DoD] the latched completion goes with the status bit
        mask ^= ((value >> 16) & 0x3FFu);

        uint32_t next = (current & ~((0x3FFu) | (0x3FFu << 16) | (1u << 31)));
        next |= status | (mask << 16);
        if ((status & mask) != 0u)
            next |= (1u << 31);
        m_ioRegisters[address] = next;
        return true;
    }

    if (address == 0x1000B400u && g_dodMpegToIpu.active())
    {
        if ((value & 0x100u) == 0u)
            g_dodMpegToIpu.abort();
        else
        {
            // Rewriting STR on this still-pending transaction is a retry, not a
            // second packet. Do not gather from a buffer the guest may recycle.
            if (dodServiceMpegToIpu(m_ioRegisters)) completeDmacChannel(address, 4u);
            return true;
        }
    }
    m_ioRegisters[address] = value;
    // [DoD] D_ENABLEW (0x1000F590) and D_ENABLER (0x1000F520) are the write and read ports of the one DMAC suspend
    // register (upstream PS2Recomp issue #257).
    if (address == 0x1000F590u)
        m_ioRegisters[0x1000F520u] = value;

    if (address >= 0x10003C00u && address < 0x10003E00u)
    {
        m_vifWriteCount.fetch_add(1, std::memory_order_relaxed);

        switch (address)
        {
        case 0x10003C10u:     // VIF1_FBRST
            dodVif1Event("FBRST", value, vif1_regs.cycle);
            if (value & 0x1u) // RST
            {
                const bool wasPath3Masked = m_path3Masked;
                // [DoD] As PCSX2 (Vif.cpp vif1FBRST): RST resets the VIF's state - the command in progress, STAT,
                // ERR, MSKPATH3 - but not its registers: CYCLE, MODE, MASK, ROW / COL, BASE / OFST / TOPS and ITOPS
                // stay. Clearing them all made an UNPACK sent after a reset without its own STCYCL read no data
                // (CL = WL = 0), and the vectors were then run as VIFcodes: stray DIRECT / MSCAL / MPG, PCSX2's
                // texture cache filled with targets 2048 wide, a black picture (rooms 0x2CA, 0x3C1, 0x3C2, 0x3C5).
                vif1_regs.stat = 0u;
                vif1_regs.err = 0u;
                dodVif1ResetCommand();
                m_vif1PendingPath2ImageQwc = 0u;
                m_vif1PendingPath2DirectHl = false;
                m_path3Masked = false;
                if (wasPath3Masked)
                    flushMaskedPath3Packets();
            }
            if (value & 0x8u) // STC
            {
                vif1_regs.stat &= ~((1u << 8) | (1u << 9) | (1u << 10) | (1u << 11) | (1u << 12) | (1u << 13));
            }
            break;
        case 0x10003C20u: // [DoD] VIF1_ERR: MII / ME0 / ME1 (ME1 masks the undefined-VIFcode error)
            vif1_regs.err = value & 0x7u;
            break;
        case 0x10003C30u:
            vif1_regs.mark = value & 0xFFFFu;
            vif1_regs.stat &= ~(1u << 6); // clear MRK flag on CPU write
            break;
        case 0x10003C40u:
            dodVif1Event("CYCLE write", value, 0u);
            vif1_regs.cycle = value & 0xFFFFu;
            break;
        case 0x10003C50u:
            vif1_regs.mode = value & 0x3u;
            break;
        case 0x10003C60u:
            vif1_regs.num = value & 0xFFu;
            break;
        case 0x10003C70u:
            vif1_regs.mask = value;
            break;
        case 0x10003C80u:
            vif1_regs.code = value;
            break;
        case 0x10003C90u:
            vif1_regs.itops = value & 0x3FFu;
            break;
        case 0x10003CA0u:
            vif1_regs.base = value & 0x3FFu;
            break;
        case 0x10003CB0u:
            vif1_regs.ofst = value & 0x3FFu;
            break;
        case 0x10003CC0u:
            vif1_regs.tops = value & 0x3FFu;
            break;
        case 0x10003CD0u:
            vif1_regs.itop = value & 0x3FFu;
            break;
        case 0x10003CE0u:
            vif1_regs.top = value & 0x3FFu;
            break;
        default:
            break;
        }

        return true;
    }

    if (address >= 0x10003800u && address < 0x10003A00u)
    {
        m_vifWriteCount.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    if (address >= 0x10008000 && address < 0x1000F000)
    {
        if ((address & 0xFF) == 0x00 && (value & 0x100))
        {
            const auto dctrlIt = m_ioRegisters.find(0x1000E000u);
            const bool dmacEnabled = (dctrlIt == m_ioRegisters.end()) || ((dctrlIt->second & 0x1u) != 0u);
            if (!dmacEnabled)
            {
                return true;
            }

            const uint32_t channelBase = address & 0xFFFFFF00;
            uint32_t madr = m_ioRegisters[channelBase + 0x10];
            if (madr & 0x80000000u) // [DoD] MADR.SPR: scratchpad
                madr = 0x70000000u | (madr & 0x3FF0u);
            const uint32_t qwc = m_ioRegisters[channelBase + 0x20];
            m_dmaStartCount.fetch_add(1, std::memory_order_relaxed);
            ++g_dodDmaStarts[(channelBase >> 10) & 0x3Fu][value & 1u]; // [DoD] diagnostics: starts per channel/direction
            {
                static uint32_t s_dodDmaLogged = 0u; // [DoD] diagnostics: first DMA starts outside the GIF/VIF paths
                if (s_dodDmaLogged < 40u && channelBase != 0x10009000u && channelBase != 0x1000A000u)
                {
                    ++s_dodDmaLogged;
                    std::fprintf(stderr, "[dma] start ch=%08X chcr=%08X madr=%08X qwc=%u tadr=%08X\n", channelBase, value, madr, qwc,
                                 m_ioRegisters[channelBase + 0x30]);
                }
            }

            // [DoD] IPU channels: toIPU (4) feeds the decoder input, fromIPU (3) takes its output when there is some.
            if (channelBase == 0x1000B400u)
            {
                const auto session = dod_mpeg_dma::activeSession();
                if (session)
                {
                    g_dodMpegToIpu.stage(session, dod_mpeg_dma::readRegisters(m_ioRegisters), value,
                        [&](uint32_t source) { return dodDmaPointer(source, m_rdram, m_scratchpad); }, dodDmaAdvance);
                    if (dodServiceMpegToIpu(m_ioRegisters)) completeDmacChannel(channelBase, 4u);
                    return true; // Exclusive MPEG ownership; never feed these bytes to IDEC too.
                }
                if (dodac5ipu::enabled())
                {
                    dodac5ipu::attach(*this);
                    dodac5ipu::g_tagEnd = false;
                    dodac5ipu::g_pullLeft = 8u; // the IPU input FIFO: the rest is pulled by the commands that need it
                    dodac5ipu::toAdvance(*this);
                    dodac5ipu::g_pullLeft = 0u;
                    return true;
                }
                const std::vector<uint8_t> input = dodGatherToIpu(m_ioRegisters, value, m_rdram, m_scratchpad);
                completeDmacChannel(channelBase, 4u);
                if (!input.empty())
                    ps2_ipu::pushInput(input.data(), input.size());
                if (dodServiceIpuFrom(m_ioRegisters, m_rdram, m_scratchpad))
                    completeDmacChannel(0x1000B000u, 3u);
                return true;
            }
            if (channelBase == 0x1000B000u && dodac5ipu::enabled())
            {
                dodac5ipu::attach(*this);
                dodac5ipu::fromAdvance(*this);
                return true;
            }
            if (channelBase == 0x1000B000u)
            {
                g_dodIpuFrom.active = true;
                g_dodIpuFrom.madr = m_ioRegisters[channelBase + 0x10];
                g_dodIpuFrom.qwc = qwc & 0xFFFFu;
                if (dodServiceIpuFrom(m_ioRegisters, m_rdram, m_scratchpad))
                    completeDmacChannel(channelBase, 3u);
                return true;
            }

            if (tryProcessScratchpadDma(channelBase, value))
            {
                return true;
            }

            if ((channelBase == 0x1000A000u || channelBase == 0x10009000u || channelBase == 0x10008000u) && (m_gsVRAM || channelBase == 0x10008000u))
            {
                auto enqueueTransfer = [&](uint32_t srcAddr, uint32_t qwCount)
                {
                    if (qwCount == 0)
                        return;
                    const bool scratch = isScratchpad(srcAddr);
                    PendingTransfer pt;
                    pt.fromScratchpad = scratch;
                    pt.srcAddr = srcAddr;
                    pt.qwc = qwCount;
                    if (channelBase == 0x1000A000u)
                        m_pendingGifTransfers.push_back(pt);
                    else if (channelBase == 0x10009000u)
                        m_pendingVif1Transfers.push_back(pt);
                    else if (channelBase == 0x10008000u)
                        m_pendingVif0Transfers.push_back(pt);
                };

                uint32_t chcr = value;
                uint32_t mode = (chcr >> 2) & 0x3;

                if (mode == 0 && qwc > 0)
                {
                    enqueueTransfer(madr, qwc);
                }
                else if (mode == 1)
                {
                    uint32_t tagAddr = m_ioRegisters[channelBase + 0x30];
                    uint32_t asr0 = m_ioRegisters[channelBase + 0x40];
                    uint32_t asr1 = m_ioRegisters[channelBase + 0x50];
                    uint32_t asp = (chcr >> 4) & 0x3u;
                    const bool tieEnabled = (chcr & (1u << 7)) != 0u;
                    // [DoD] Only a runaway guard (KillzoneRecomp patch 0010: a gameplay frame can be one VIF1 chain of more
                    // than 4096 tags; the rest of the chain used to be dropped while CHCR read back as done).
                    const int kMaxChainTags = 1 << 20;
                    std::vector<uint8_t> chainBuf = dodChainAcquire();
                    static const bool s_lazyEnabled = std::getenv("DOD_NOLAZY") == nullptr; // [DoD] A/B switch
                    const bool lazyVif1 = s_lazyEnabled && channelBase == 0x10009000u; // [DoD] see DodLazyChain
                    DodLazyChain lazyChain;

                    auto appendData = [&](uint32_t srcAddr, uint32_t qwCount)
                    {
                        const uint64_t bytes64 = static_cast<uint64_t>(qwCount) * 16ull;
                        uint32_t bytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
                        const bool scratch = isScratchpad(srcAddr) || dodDmaAddressIsScratchpad(srcAddr);
                        uint32_t src = 0;
                        src = scratch ? (srcAddr & (PS2_SCRATCHPAD_SIZE - 1u)) : translateAddress(srcAddr);
                        if (lazyVif1 && bytes != 0u && bytes <= PS2_RAM_SIZE)
                        {
                            DodChainSegment seg;
                            seg.offset = static_cast<uint32_t>(chainBuf.size());
                            seg.size = bytes;
                            seg.source = src;
                            seg.scratchpad = scratch;
                            lazyChain.segments.push_back(seg);
                            chainBuf.resize(chainBuf.size() + bytes);
                            return;
                        }
                        const uint8_t *base2;
                        uint32_t maxSz2;
                        if (scratch)
                        {
                            base2 = m_scratchpad;
                            maxSz2 = PS2_SCRATCHPAD_SIZE;
                        }
                        else
                        {
                            base2 = m_rdram;
                            maxSz2 = PS2_RAM_SIZE;
                        }

                        while (bytes > 0)
                        {
                            if (src >= maxSz2)
                                src = 0;
                            uint32_t chunk = bytes;
                            if (src + chunk > maxSz2)
                                chunk = maxSz2 - src;
                            if (chunk == 0)
                                break;
                            chainBuf.insert(chainBuf.end(), base2 + src, base2 + src + chunk);
                            bytes -= chunk;
                            src += chunk;
                        }
                    };

                    auto appendVifTagData = [&](uint32_t localTagAddr)
                    {
                        uint32_t tagPhys = 0u;
                        const bool tagScratch = isScratchpad(localTagAddr);
                        tagPhys = translateAddress(localTagAddr);

                        const uint8_t *localBase = tagScratch ? m_scratchpad : m_rdram;
                        const uint32_t localMax = tagScratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
                        if (tagPhys + 16u > localMax)
                            return;

                        // CHCR.TTE sends the DMAtag's upper 64 bits to the channel before
                        // the tag payload. VIF chains use those bytes for two VIFcodes.
                        if (lazyVif1)
                        {
                            DodChainSegment seg;
                            seg.offset = static_cast<uint32_t>(chainBuf.size());
                            seg.size = 8u;
                            seg.literal = true;
                            lazyChain.segments.push_back(seg);
                        }
                        chainBuf.insert(chainBuf.end(), localBase + tagPhys + 8u, localBase + tagPhys + 16u);
                    };

                    const bool isVifChannel =
                        channelBase == 0x10009000u || channelBase == 0x10008000u;
                    const bool transferTagData = isVifChannel && (chcr & 0x40u) != 0u;

                    int tagsProcessed = 0;
                    uint32_t lastTagUpper = (chcr >> 16) & 0xFFFFu;
                    // [DoD] DOD_VIF_LISTCHECK=1: does the game change a VIF1 chain's first 34 qwords (DoD's 33-layer master list
                    // at 0x819AD0 / 0x819CE0) after starting the DMA? Each start keeps a copy; the next start compares the
                    // previous chain's start with memory (a frame later, before that list is rebuilt).
                    if (channelBase == 0x10009000u)
                    {
                        static const bool s_listCheck = std::getenv("DOD_VIF_LISTCHECK") != nullptr;
                        static uint32_t s_prevAddr = 0u;
                        static uint8_t s_prev[34 * 16];
                        static int s_reports = 0;
                        if (s_listCheck)
                        {
                            if (s_prevAddr != 0u && s_reports < 40)
                            {
                                const uint32_t phys = translateAddress(s_prevAddr);
                                if (phys + sizeof(s_prev) <= PS2_RAM_SIZE && std::memcmp(m_rdram + phys, s_prev, sizeof(s_prev)) != 0)
                                {
                                    ++s_reports;
                                    for (uint32_t q = 0; q < 34u; ++q)
                                    {
                                        if (std::memcmp(m_rdram + phys + q * 16u, s_prev + q * 16u, 16u) == 0)
                                            continue;
                                        uint32_t was[4], now[4];
                                        std::memcpy(was, s_prev + q * 16u, 16u);
                                        std::memcpy(now, m_rdram + phys + q * 16u, 16u);
                                        std::fprintf(stderr, "[vif1-list] %08X entry %u changed after the DMA start: %08X %08X -> %08X %08X" "\n",
                                                     s_prevAddr, q, was[0], was[1], now[0], now[1]);
                                    }
                                }
                            }
                            const uint32_t phys = translateAddress(tagAddr);
                            if (phys + sizeof(s_prev) <= PS2_RAM_SIZE)
                            {
                                std::memcpy(s_prev, m_rdram + phys, sizeof(s_prev));
                                s_prevAddr = tagAddr;
                            }
                        }
                    }
                    // [DoD] DOD_CHAIN_DUMP=<bytes>: the tags of a VIF1 chain that collects more than that many bytes
                    // (a chain walk that runs past its end shows as a huge transfer).
                    static const uint32_t s_chainDumpBytes = []() { const char *v = std::getenv("DOD_CHAIN_DUMP"); return v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 0)) : 0u; }();
                    static const bool s_keepNotes = s_chainDumpBytes != 0u || std::getenv("DOD_VIF_LOG") != nullptr;
                    std::vector<DodTagNote> &tagNotes = g_dodLastVif1ChainNotes;
                    if (channelBase == 0x10009000u)
                    {
                        // [DoD] DOD_VIF_LISTCHECK: the previous VIF1 chain's tags, compared with memory now (did the game
                        // write a tag of a chain after starting its DMA?).
                        static const bool s_tagCheck = std::getenv("DOD_VIF_LISTCHECK") != nullptr;
                        static int s_tagReports = 0;
                        if (s_tagCheck && s_tagReports < 60)
                        {
                            int changed = 0;
                            for (size_t k = 0; k < tagNotes.size() && s_tagReports < 60; ++k)
                            {
                                const DodTagNote &t = tagNotes[k];
                                const bool spr = isScratchpad(t.at);
                                if (spr)
                                    continue;
                                const uint32_t phys = translateAddress(t.at);
                                if (phys + 16u > PS2_RAM_SIZE)
                                    continue;
                                uint32_t now[4];
                                std::memcpy(now, m_rdram + phys, 16u);
                                if (now[0] == t.id && now[1] == t.addr && now[2] == t.hi0 && now[3] == t.hi1)
                                    continue;
                                if (changed++ < 3)
                                {
                                    ++s_tagReports;
                                    std::fprintf(stderr, "[vif1-tag] tag #%zu of %zu at %08X changed after its DMA start: %08X %08X %08X %08X -> %08X %08X %08X %08X" "\n",
                                                 k, tagNotes.size(), t.at, t.id, t.addr, t.hi0, t.hi1, now[0], now[1], now[2], now[3]);
                                }
                            }
                            if (changed > 3)
                                std::fprintf(stderr, "[vif1-tag]   %d tags of that chain changed" "\n", changed);
                        }
                        tagNotes.clear();
                    }

                    while (tagsProcessed < kMaxChainTags)
                    {
                        const uint32_t currentTagAddr = tagAddr;
                        const bool tagInSPR = isScratchpad(tagAddr);
                        uint32_t physTag = 0;
                        try
                        {
                            physTag = translateAddress(tagAddr);
                        }
                        catch (...)
                        {
                            break;
                        }
                        const uint8_t *tagBase;
                        uint32_t tagMax;
                        if (tagInSPR)
                        {
                            tagBase = m_scratchpad;
                            tagMax = PS2_SCRATCHPAD_SIZE;
                        }
                        else
                        {
                            tagBase = m_rdram;
                            tagMax = PS2_RAM_SIZE;
                        }
                        if (physTag + 16 > tagMax)
                            break;

                        const uint8_t *tp = tagBase + physTag;
                        uint64_t tag = loadScalar<uint64_t>(tp, 0, 16, "dma chain tag", tagAddr);
                        uint16_t tagQwc = static_cast<uint16_t>(tag & 0xFFFF);
                        uint32_t id = static_cast<uint32_t>((tag >> 28) & 0x7);
                        const bool irq = ((tag >> 31) & 0x1ull) != 0ull;
                        // [DoD] ADDR bit 31 (SPR) selects the scratchpad: map it to the CPU scratchpad window.
                        uint32_t addr = static_cast<uint32_t>(tag >> 32);
                        addr = (addr & 0x80000000u) ? (0x70000000u | (addr & 0x3FF0u)) : (addr & 0x7FFFFFFFu);
                        lastTagUpper = static_cast<uint32_t>((tag >> 16) & 0xFFFFu);
                        ++tagsProcessed;
                        {
                            static uint32_t s_dodTagsLogged = 0u; // [DoD] diagnostics: first large chain payloads
                            if (s_dodTagsLogged < 60u && channelBase == 0x10009000u && tagQwc >= 64u)
                            {
                                ++s_dodTagsLogged;
                                std::fprintf(stderr, "[dma]   tag@%08X id=%u qwc=%u addr=%08X%s\n", currentTagAddr, id, tagQwc,
                                             static_cast<uint32_t>(tag >> 32), tagInSPR ? " (tag in SPR)" : "");
                            }
                        }
                        if (channelBase == 0x1000A000u || channelBase == 0x10009000u)
                        {
                            // [DoD] DOD_GIF_TAGLOG=<first field>:<fields>: every GIF (ch2) / VIF1 (ch1) chain tag in those fields
                            // (where a transfer's data comes from in EE memory; field = dodpcsx2::presents()).
                            static uint64_t s_first = 0u, s_last = 0u;
                            static const bool s_on = []() {
                                const char *v = std::getenv("DOD_GIF_TAGLOG");
                                if (!v)
                                    return false;
                                char *end = nullptr;
                                s_first = std::strtoull(v, &end, 10);
                                s_last = s_first + (end && *end == ':' ? std::strtoull(end + 1, nullptr, 10) : 1u);
                                return true;
                            }();
                            static uint32_t s_lines = 0u;
                            if (s_on && s_lines < 40000u)
                            {
                                const uint64_t field = dodpcsx2::presents();
                                if (field >= s_first && field < s_last)
                                {
                                    ++s_lines;
                                    std::fprintf(stderr, "[gif-tag] f%llu ch%u tag@%08X id=%u qwc=%u addr=%08X\n",
                                                 static_cast<unsigned long long>(field),
                                                 channelBase == 0x1000A000u ? 2u : 1u, currentTagAddr, id, tagQwc,
                                                 static_cast<uint32_t>(tag >> 32));
                                }
                            }
                        }

                        if (s_keepNotes && channelBase == 0x10009000u)
                            tagNotes.push_back({currentTagAddr, static_cast<uint32_t>(tag), tagQwc, static_cast<uint32_t>(tag >> 32),
                                                loadScalar<uint32_t>(tp, 8, 16, "dma chain tag", tagAddr),
                                                loadScalar<uint32_t>(tp, 12, 16, "dma chain tag", tagAddr), asp,
                                                static_cast<uint32_t>(chainBuf.size())});

                        uint32_t dataAddr = 0;
                        bool hasPayload = (tagQwc > 0);
                        bool endChain = false;

                        switch (id)
                        {
                        case 0:
                            dataAddr = addr;
                            tagAddr = tagAddr + 16;
                            endChain = true;
                            break;
                        case 1:
                            dataAddr = tagAddr + 16;
                            tagAddr = dataAddr + static_cast<uint32_t>(tagQwc) * 16u;
                            break;
                        case 2:
                            dataAddr = tagAddr + 16;
                            tagAddr = addr;
                            break;
                        case 3:
                        case 4:
                            dataAddr = addr;
                            tagAddr = tagAddr + 16;
                            break;
                        case 5:
                            dataAddr = tagAddr + 16;
                            {
                                const uint32_t retAddr = dataAddr + static_cast<uint32_t>(tagQwc) * 16u;
                                if (asp == 0u)
                                {
                                    asr0 = retAddr;
                                    asp = 1u;
                                }
                                else if (asp == 1u)
                                {
                                    asr1 = retAddr;
                                    asp = 2u;
                                }
                                else
                                {
                                    // [DoD] A third nested call: the address stack holds two. PCSX2 (hwDmacSrcChainWithStack,
                                    // "Call Stack Overflow") sends the tag's data and ends the transfer there (TADR stays on
                                    // the tag); jumping on without saving the return address sent a later RET back to the
                                    // wrong place.
                                    static uint32_t s_overflows = 0u;
                                    if (s_overflows++ < 20u)
                                        std::fprintf(stderr, "[dma] ch%u call stack overflow: CALL at %08X (to %08X) with ASP 2; transfer ended as PCSX2" "\n",
                                                     channelBase == 0x10009000u ? 1u : (channelBase == 0x1000A000u ? 2u : 0u), currentTagAddr, addr);
                                    endChain = true;
                                    break;
                                }
                            }
                            tagAddr = addr;
                            break;
                        case 6:
                            dataAddr = tagAddr + 16;
                            if (asp == 2u)
                            {
                                tagAddr = asr1;
                                asp = 1u;
                            }
                            else if (asp == 1u)
                            {
                                tagAddr = asr0;
                                asp = 0u;
                            }
                            else
                            {
                                endChain = true;
                            }
                            break;
                        case 7:
                            dataAddr = tagAddr + 16;
                            endChain = true;
                            break;
                        default:
                            hasPayload = false;
                            endChain = true;
                            break;
                        }

                        if (transferTagData)
                            appendVifTagData(currentTagAddr);

                        if (hasPayload)
                            appendData(dataAddr, tagQwc);
                        if (irq && tieEnabled)
                            endChain = true;
                        if (endChain)
                            break;
                    }

                    if (s_chainDumpBytes != 0u && chainBuf.size() > s_chainDumpBytes)
                    {
                        static int s_dumps = 0;
                        if (s_dumps++ < 3)
                        {
                            std::fprintf(stderr, "[dma-chain] ch1 chain from %08X: %zu bytes, %d tags (CHCR %08X)" "\n",
                                         m_ioRegisters[channelBase + 0x30], chainBuf.size(), tagsProcessed, chcr);
                            for (size_t k = 0; k < tagNotes.size(); ++k)
                            {
                                if (s_dumps > 1 && k >= 120u && k + 120u < tagNotes.size())
                                    continue; // the first dump lists every tag
                                const DodTagNote &t = tagNotes[k];
                                std::fprintf(stderr, "[dma-chain]   #%zu +%u tag@%08X lo=%08X qwc=%u addr=%08X vif %08X %08X asp %u" "\n", k, t.off,
                                             t.at, t.id, t.qwc, t.addr, t.hi0, t.hi1, t.asp);
                            }
                        }
                    }
                    if (tagsProcessed >= kMaxChainTags)
                    {
                        static uint32_t s_dodCapLogged = 0u; // [DoD] diagnostics
                        if (s_dodCapLogged < 10u)
                        {
                            ++s_dodCapLogged;
                            std::fprintf(stderr, "[dma] chain on %08X stopped at the %d-tag limit, next tag %08X\n", channelBase,
                                         kMaxChainTags, tagAddr);
                        }
                    }
                    m_ioRegisters[channelBase + 0x30] = tagAddr;
                    m_ioRegisters[channelBase + 0x40] = asr0;
                    m_ioRegisters[channelBase + 0x50] = asr1;
                    chcr = (chcr & ~(0x3u << 4)) | ((asp & 0x3u) << 4);
                    chcr = (chcr & 0x0000FFFFu) | (lastTagUpper << 16);
                    m_ioRegisters[channelBase + 0x00] = chcr;

                    if (!chainBuf.empty())
                    {
                        PendingTransfer pt;
                        pt.fromScratchpad = false;
                        pt.srcAddr = 0;
                        pt.qwc = 0;
                        pt.chainData = std::move(chainBuf);
                        if (lazyVif1 && !lazyChain.segments.empty())
                            g_dodLazyChains[pt.chainData.data()] = std::move(lazyChain);
                        if (channelBase == 0x1000A000)
                        {
                            m_pendingGifTransfers.push_back(std::move(pt));
                        }
                        else if (channelBase == 0x10009000u)
                        {
                            m_pendingVif1Transfers.push_back(std::move(pt));
                        }
                        else if (channelBase == 0x10008000u)
                        {
                            m_pendingVif0Transfers.push_back(std::move(pt));
                        }
                    }
                    // else if (channelBase == 0x10009000u)
                    // {

                    // }
                }
                else if (qwc > 0)
                {
                    enqueueTransfer(madr, qwc);
                }

                const bool autoProcessTransfers =
                    (channelBase == 0x1000A000u) ? (m_gifPacketCallback || m_gifArbiter != nullptr) : true;
                if (autoProcessTransfers)
                {
                    processPendingTransfers();
                }
            }
        }
        return true;
    }

    if (address >= 0x10000000 && address < 0x10010000)
    {
        if (address >= 0x10000200 && address < 0x10000300)
        {
            return true;
        }
        if (address >= 0x10000000 && address < 0x10000100)
        {
            return true;
        }
    }

    return false;
}

bool PS2Memory::tryProcessScratchpadDma(uint32_t channelBase, uint32_t chcr)
{
    static constexpr uint32_t kSprFromChannel = 0x1000D000u;
    static constexpr uint32_t kSprToChannel = 0x1000D400u;
    if (channelBase != kSprFromChannel && channelBase != kSprToChannel)
        return false;

    const uint32_t mode = (chcr >> 2u) & 0x3u;
    static const bool sprChain = std::getenv("DOD_SPR_CHAIN") == nullptr; // [DoD] read once (hot path)
    if (mode == 1u && sprChain)
    {
        // [DoD] Chain mode (EE User's Manual 5.4/5.6, PCSX2 SPR.cpp), from KillzoneRecomp patch 0010; MPEG P/B frames
        // fetch their reference macroblocks with it (Urban Reign port, PROGRESS 89). DOD_SPR_CHAIN=off disables it.
        //  toSPR (ch9) is a source chain: DMAtags are read from main memory at TADR (REFE/CNT/NEXT/REF/REFS/CALL/RET/
        //  END); with CHCR.TTE the whole tag is written to the scratchpad before its data.
        //  fromSPR (ch8) is a destination chain: each DMAtag is read from the scratchpad at SADR (CNTS/CNT/END); its data
        //  follows it in the scratchpad and goes to main memory at the tag's ADDR.
        // Both stop after END, REFE or RET with an empty call stack, or after a tag with IRQ set when CHCR.TIE is set.
        const bool fromScratchpad = channelBase == kSprFromChannel;
        uint32_t sadr = m_ioRegisters[channelBase + 0x80u] & 0x3FF0u;
        uint32_t tadr = m_ioRegisters[channelBase + 0x30u];
        uint32_t madr = m_ioRegisters[channelBase + 0x10u];
        uint32_t asr[2] = {m_ioRegisters[channelBase + 0x40u], m_ioRegisters[channelBase + 0x50u]};
        uint32_t asp = (chcr >> 4u) & 3u;
        const bool tte = (chcr & 0x40u) != 0u;
        const bool tie = (chcr & 0x80u) != 0u;
        uint32_t lastTagUpper = chcr >> 16u;

        auto mainPtr = [&](uint32_t addr, uint32_t bytes) -> uint8_t *
        {
            const uint32_t phys = addr & 0x01FFFFF0u;
            if (phys >= PS2_RAM_SIZE || bytes > PS2_RAM_SIZE - phys)
                return nullptr;
            return m_rdram + phys;
        };
        auto toSpr = [&](const uint8_t *src, uint32_t bytes)
        {
            while (bytes != 0u)
            {
                const uint32_t chunk = std::min<uint32_t>(bytes, PS2_SCRATCHPAD_SIZE - sadr);
                std::memcpy(m_scratchpad + sadr, src, chunk);
                src += chunk;
                bytes -= chunk;
                sadr = (sadr + chunk) & (PS2_SCRATCHPAD_SIZE - 1u);
            }
        };
        auto fromSpr = [&](uint32_t dstAddr, uint32_t bytes)
        {
            uint8_t *dst = mainPtr(dstAddr, bytes);
            if (!dst)
            {
                sadr = (sadr + bytes) & (PS2_SCRATCHPAD_SIZE - 1u);
                return;
            }
            markModified(dstAddr & 0x01FFFFF0u, bytes);
            while (bytes != 0u)
            {
                const uint32_t chunk = std::min<uint32_t>(bytes, PS2_SCRATCHPAD_SIZE - sadr);
                std::memcpy(dst, m_scratchpad + sadr, chunk);
                dst += chunk;
                bytes -= chunk;
                sadr = (sadr + chunk) & (PS2_SCRATCHPAD_SIZE - 1u);
            }
        };

        // A chain start with QWC != 0 first transfers MADR/QWC.
        const uint32_t startQwc = m_ioRegisters[channelBase + 0x20u] & 0xFFFFu;
        if (startQwc != 0u)
        {
            if (fromScratchpad)
                fromSpr(madr, startQwc * 16u);
            else if (const uint8_t *mem = mainPtr(madr, startQwc * 16u))
                toSpr(mem, startQwc * 16u);
            madr += startQwc * 16u;
        }

        for (int tags = 0; tags < 16384; ++tags)
        {
            uint64_t tag[2] = {0u, 0u};
            if (fromScratchpad)
            {
                std::memcpy(tag, m_scratchpad + sadr, 16u);
                sadr = (sadr + 16u) & (PS2_SCRATCHPAD_SIZE - 1u);
            }
            else
            {
                const bool tagInSpr = (tadr & 0x80000000u) != 0u || (tadr & 0xF0000000u) == 0x70000000u;
                const uint8_t *tp = tagInSpr ? m_scratchpad + (tadr & 0x3FF0u) : mainPtr(tadr, 16u);
                if (!tp)
                    break;
                std::memcpy(tag, tp, 16u);
            }

            const uint32_t tagQwc = static_cast<uint32_t>(tag[0] & 0xFFFFu);
            const uint32_t id = static_cast<uint32_t>((tag[0] >> 28) & 7u);
            const bool irq = ((tag[0] >> 31) & 1u) != 0u;
            const uint32_t addr = static_cast<uint32_t>(tag[0] >> 32);
            lastTagUpper = static_cast<uint32_t>((tag[0] >> 16) & 0xFFFFu);
            bool end = false;

            if (fromScratchpad)
            {
                madr = addr;
                if (id == 7u)
                    end = true;
                if (tagQwc != 0u)
                    fromSpr(madr, tagQwc * 16u);
                madr += tagQwc * 16u;
            }
            else
            {
                uint32_t dataAddr = tadr + 16u;
                switch (id)
                {
                case 0u: // REFE
                    dataAddr = addr;
                    tadr += 16u;
                    end = true;
                    break;
                case 1u: // CNT
                    tadr = dataAddr + tagQwc * 16u;
                    break;
                case 2u: // NEXT
                    tadr = addr;
                    break;
                case 3u: // REF
                case 4u: // REFS
                    dataAddr = addr;
                    tadr += 16u;
                    break;
                case 5u: // CALL
                    if (asp < 2u)
                        asr[asp++] = dataAddr + tagQwc * 16u;
                    tadr = addr;
                    break;
                case 6u: // RET
                    if (asp > 0u)
                        tadr = asr[--asp];
                    else
                        end = true;
                    break;
                default: // END
                    end = true;
                    break;
                }
                if (tte)
                    toSpr(reinterpret_cast<const uint8_t *>(tag), 16u);
                if (tagQwc != 0u)
                {
                    if (const uint8_t *mem = mainPtr(dataAddr, tagQwc * 16u))
                        toSpr(mem, tagQwc * 16u);
                    else
                        sadr = (sadr + tagQwc * 16u) & (PS2_SCRATCHPAD_SIZE - 1u);
                }
                madr = dataAddr + tagQwc * 16u;
            }
            if (irq && tie)
                end = true;
            if (end)
                break;
        }

        m_ioRegisters[channelBase + 0x10u] = madr;
        m_ioRegisters[channelBase + 0x20u] = 0u;
        m_ioRegisters[channelBase + 0x30u] = tadr;
        m_ioRegisters[channelBase + 0x40u] = asr[0];
        m_ioRegisters[channelBase + 0x50u] = asr[1];
        m_ioRegisters[channelBase + 0x80u] = sadr;
        m_ioRegisters[channelBase] = (chcr & 0x0000FFCFu) | ((asp & 3u) << 4u) | (lastTagUpper << 16u);
        completeDmacChannel(channelBase, fromScratchpad ? 8u : 9u);
        return true;
    }
    if (mode == 2u)
    {
        // [DoD] Interleave mode: the scratchpad side is contiguous; on the memory side D_SQWC.TQWC quadwords are
        // transferred, then D_SQWC.SQWC are skipped (Dawn of Dreams rasterises IPU macroblocks this way).
        const uint32_t sqwc = m_ioRegisters[0x1000E030u] & 0xFFu;
        const uint32_t tqwc = std::max<uint32_t>((m_ioRegisters[0x1000E030u] >> 16) & 0xFFu, 1u);
        uint32_t qwcLeft = m_ioRegisters[channelBase + 0x20u] & 0xFFFFu;
        uint32_t madr = m_ioRegisters[channelBase + 0x10u] & 0x7FFFFFF0u;
        uint32_t sadr = m_ioRegisters[channelBase + 0x80u] & 0x3FF0u;
        const bool fromScratchpad = channelBase == kSprFromChannel;
        uint32_t inBlock = 0u;
        while (qwcLeft != 0u)
        {
            const uint32_t mainOffset = translateAddress(madr) & (PS2_RAM_SIZE - 16u);
            if (fromScratchpad)
            {
                std::memcpy(m_rdram + mainOffset, m_scratchpad + sadr, 16u);
                markModified(mainOffset, 16u);
            }
            else
            {
                std::memcpy(m_scratchpad + sadr, m_rdram + mainOffset, 16u);
            }
            sadr = (sadr + 16u) & (PS2_SCRATCHPAD_SIZE - 16u);
            madr += 16u;
            --qwcLeft;
            if (++inBlock == tqwc)
            {
                inBlock = 0u;
                madr += sqwc * 16u;
            }
        }
        m_ioRegisters[channelBase + 0x10u] = madr;
        m_ioRegisters[channelBase + 0x20u] = 0u;
        m_ioRegisters[channelBase + 0x80u] = sadr;
        completeDmacChannel(channelBase, fromScratchpad ? 8u : 9u);
        return true;
    }
    if (mode != 0u)
        return false;

    const uint32_t qwc = m_ioRegisters[channelBase + 0x20u] & 0xFFFFu;
    const uint32_t byteCount = qwc * 16u;
    const uint32_t originalMadr = m_ioRegisters[channelBase + 0x10u] & 0x7FFFFFF0u;
    const uint32_t originalSadr = m_ioRegisters[channelBase + 0x80u] & 0x3FF0u;

    uint32_t mainOffset = 0u;
    try
    {
        mainOffset = translateAddress(originalMadr);
    }
    catch (const std::exception &)
    {
        return false;
    }

    if (mainOffset > PS2_RAM_SIZE || byteCount > PS2_RAM_SIZE - mainOffset)
        return false;

    const bool fromScratchpad = channelBase == kSprFromChannel;
    uint32_t scratchOffset = originalSadr;
    uint32_t bytesLeft = byteCount;
    uint32_t copied = 0u;
    while (bytesLeft != 0u)
    {
        const uint32_t scratchChunk = PS2_SCRATCHPAD_SIZE - scratchOffset;
        const uint32_t chunk = std::min(bytesLeft, scratchChunk);
        if (fromScratchpad)
        {
            std::memcpy(m_rdram + mainOffset + copied, m_scratchpad + scratchOffset, chunk);
            markModified(mainOffset + copied, chunk);
        }
        else
        {
            std::memcpy(m_scratchpad + scratchOffset, m_rdram + mainOffset + copied, chunk);
        }

        copied += chunk;
        bytesLeft -= chunk;
        scratchOffset = (scratchOffset + chunk) & (PS2_SCRATCHPAD_SIZE - 1u);
    }

    m_ioRegisters[channelBase + 0x10u] = (originalMadr + byteCount) & 0x7FFFFFF0u;
    m_ioRegisters[channelBase + 0x20u] = 0u;
    m_ioRegisters[channelBase + 0x80u] = (originalSadr + byteCount) & 0x3FF0u;
    completeDmacChannel(channelBase, fromScratchpad ? 8u : 9u);
    return true;
}

void PS2Memory::completeDmacChannel(uint32_t channelBase, uint32_t cause)
{
    static constexpr uint32_t kDStat = 0x1000E010u;
    m_ioRegisters[channelBase] &= ~0x100u;

    uint32_t dstat = m_ioRegisters.count(kDStat) ? m_ioRegisters[kDStat] : 0u;
    dstat |= 1u << cause;
    const uint32_t status = dstat & 0x3FFu;
    const uint32_t mask = (dstat >> 16u) & 0x3FFu;
    if ((status & mask) != 0u)
        dstat |= 1u << 31u;
    else
        dstat &= ~(1u << 31u);
    m_ioRegisters[kDStat] = dstat;
    queueCompletedDmacCause(cause);
}

void PS2Memory::processPendingTransfers()
{
    // [DoD] A GS SIGNAL interrupt handler runs in the middle of a VIF1 transfer and may start other DMAs; their
    // pending work is picked up by the next call instead of re-entering this one.
    if (g_dodInPendingTransfers)
        return;
    g_dodInPendingTransfers = true;
    struct PendingGuard
    {
        ~PendingGuard() { g_dodInPendingTransfers = false; }
    } pendingGuard;

    if (dodServiceMpegToIpu(m_ioRegisters))
        completeDmacChannel(0x1000B400u, 4u);

    const bool hadGif = !m_pendingGifTransfers.empty();
    uint32_t observedGifQwc = 0u;
    for (const auto &transfer : m_pendingGifTransfers)
    {
        const uint64_t transferQwc = !transfer.chainData.empty()
                                         ? (transfer.chainData.size() / 16u)
                                         : transfer.qwc;
        observedGifQwc = static_cast<uint32_t>(std::min<uint64_t>(16u, static_cast<uint64_t>(observedGifQwc) + transferQwc));
    }
    if (observedGifQwc != 0u)
    {
        constexpr uint32_t kGifStat = 0x10003020u;
        constexpr uint32_t kGifFqcMask = 0x1F000000u;
        dodSettleEeTimersNow(*this); // [DoD] cycles reported before this must not expire the new FQC
        uint32_t &gifStat = m_ioRegisters[kGifStat];
        gifStat = (gifStat & ~kGifFqcMask) | (observedGifQwc << 24u);
    }

    for (size_t idx = 0; idx < m_pendingGifTransfers.size(); ++idx)
    {
        auto &p = m_pendingGifTransfers[idx];
        if (!p.chainData.empty())
        {
            m_seenGifCopy = true;
            m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
            submitGifPacket(GifPathId::Path3, p.chainData.data(), static_cast<uint32_t>(p.chainData.size()), false);
        }
        else if (p.qwc > 0)
        {
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            uint32_t srcPhys = 0;
            try
            {
                srcPhys = translateAddress(p.srcAddr);
            }
            catch (const std::exception &)
            {
                continue;
            }
            if (p.fromScratchpad)
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft >= 16)
                {
                    if (srcPhys >= PS2_SCRATCHPAD_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_SCRATCHPAD_SIZE)
                        chunk = PS2_SCRATCHPAD_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    m_seenGifCopy = true;
                    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
                    submitGifPacket(GifPathId::Path3, m_scratchpad + srcPhys, chunk, false);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
            else
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft >= 16)
                {
                    if (srcPhys >= PS2_RAM_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_RAM_SIZE)
                        chunk = PS2_RAM_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    m_seenGifCopy = true;
                    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
                    submitGifPacket(GifPathId::Path3, m_rdram + srcPhys, chunk, false);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
        }
    }
    for (auto &p : m_pendingGifTransfers)
        dodChainRecycle(std::move(p.chainData));
    m_pendingGifTransfers.clear();

    const bool hadVif0 = !m_pendingVif0Transfers.empty();
    for (auto &p : m_pendingVif0Transfers)
    {
        if (!p.chainData.empty())
        {
            processVIF0Data(p.chainData.data(), static_cast<uint32_t>(p.chainData.size()));
        }
        else if (p.qwc > 0)
        {
            uint32_t srcPhys = 0;
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            try
            {
                srcPhys = translateAddress(p.srcAddr);
            }
            catch (const std::exception &)
            {
                continue;
            }
            if (p.fromScratchpad)
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_SCRATCHPAD_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_SCRATCHPAD_SIZE)
                        chunk = PS2_SCRATCHPAD_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF0Data(m_scratchpad + srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
            else
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_RAM_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_RAM_SIZE)
                        chunk = PS2_RAM_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF0Data(srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
        }
    }
    for (auto &p : m_pendingVif0Transfers)
        dodChainRecycle(std::move(p.chainData));
    m_pendingVif0Transfers.clear();

    const bool hadVif1 = !m_pendingVif1Transfers.empty();
    std::vector<PendingTransfer> vif1Transfers = std::move(m_pendingVif1Transfers); // [DoD] see the guard above
    m_pendingVif1Transfers.clear();
    for (auto &p : vif1Transfers)
    {
        if (!p.chainData.empty())
        {
            // [DoD] Chain data is copied from memory as the VIF reaches it (DodLazyChain).
            DodLazyChain *previous = g_dodActiveVif1Chain;
            const auto lazy = g_dodLazyChains.find(p.chainData.data());
            if (lazy != g_dodLazyChains.end())
            {
                lazy->second.buffer = p.chainData.data();
                lazy->second.rdram = m_rdram;
                lazy->second.scratchpad = m_scratchpad;
                g_dodActiveVif1Chain = &lazy->second;
            }
            struct ActiveChainGuard
            {
                DodLazyChain *previous;
                const uint8_t *key;
                ~ActiveChainGuard()
                {
                    g_dodActiveVif1Chain = previous;
                    g_dodLazyChains.erase(key);
                }
            } activeChainGuard{previous, p.chainData.data()};
            processVIF1Data(p.chainData.data(), static_cast<uint32_t>(p.chainData.size()));
        }
        else if (p.qwc > 0)
        {
            uint32_t srcPhys = 0;
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            try
            {
                srcPhys = translateAddress(p.srcAddr);
            }
            catch (const std::exception &)
            {
                continue;
            }
            if (p.fromScratchpad)
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_SCRATCHPAD_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_SCRATCHPAD_SIZE)
                        chunk = PS2_SCRATCHPAD_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF1Data(m_scratchpad + srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
            else
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_RAM_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_RAM_SIZE)
                        chunk = PS2_RAM_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF1Data(srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
        }
    }
    for (auto &p : vif1Transfers)
        dodChainRecycle(std::move(p.chainData)); // [DoD] chain buffer pool

    if (m_gifArbiter)
        m_gifArbiter->drain();

    static constexpr uint32_t GIF_CHANNEL = 0x1000A000;
    static constexpr uint32_t VIF0_CHANNEL = 0x10008000;
    static constexpr uint32_t VIF1_CHANNEL = 0x10009000;
    static constexpr uint32_t D_STAT = 0x1000E010u;

    auto raiseDStatChannel = [&](uint32_t channelBit)
    {
        uint32_t dstat = m_ioRegisters.count(D_STAT) ? m_ioRegisters[D_STAT] : 0u;
        dstat |= (1u << channelBit);

        const uint32_t status = dstat & 0x3FFu;
        const uint32_t mask = (dstat >> 16) & 0x3FFu;
        if ((status & mask) != 0u)
            dstat |= (1u << 31);
        else
            dstat &= ~(1u << 31);

        m_ioRegisters[D_STAT] = dstat;
    };

    if (hadGif)
    {
        raiseDStatChannel(2u); // GIF channel
        queueCompletedDmacCause(2u);
        m_ioRegisters[GIF_CHANNEL + 0x00] &= ~0x100u;
        m_ioRegisters[GIF_CHANNEL + 0x20] = 0;
    }
    if (hadVif0)
    {
        raiseDStatChannel(0u); // VIF0 channel
        queueCompletedDmacCause(0u);
        m_ioRegisters[VIF0_CHANNEL + 0x00] &= ~0x100u;
        m_ioRegisters[VIF0_CHANNEL + 0x20] = 0;
    }
    if (hadVif1)
    {
        raiseDStatChannel(1u); // VIF1 channel
        queueCompletedDmacCause(1u);
        m_ioRegisters[VIF1_CHANNEL + 0x00] &= ~0x100u;
        m_ioRegisters[VIF1_CHANNEL + 0x20] = 0;
    }
}

// [DoD] Upstream PR #343: set while completed DMAC causes wait in the queue, so the drain after every store to a
// special address costs one load when there is nothing to deliver (it took a lock and built a vector each time).
std::atomic<bool> g_dodDmacCausesPending{false};

void PS2Memory::queueCompletedDmacCause(uint32_t cause)
{
    std::lock_guard<std::mutex> lock(m_completedDmacMutex);
    m_completedDmacCauses.push_back(cause);
    g_dodDmacCausesPending.store(true, std::memory_order_release);
}

std::vector<uint32_t> PS2Memory::consumeCompletedDmacCauses()
{
    std::lock_guard<std::mutex> lock(m_completedDmacMutex);
    std::vector<uint32_t> causes;
    causes.swap(m_completedDmacCauses);
    g_dodDmacCausesPending.store(false, std::memory_order_release);
    return causes;
}

void PS2Memory::flushMaskedPath3Packets(bool drainImmediately)
{
    if (m_path3Masked || m_path3MaskedFifo.empty())
        return;

    auto emit = [&](const uint8_t *packetData, uint32_t packetSize)
    {
        if (m_gifArbiter)
            m_gifArbiter->submit(GifPathId::Path3, packetData, packetSize, false);
        else if (m_gifPacketCallback)
            m_gifPacketCallback(packetData, packetSize);
    };

    for (const auto &packet : m_path3MaskedFifo)
    {
        if (packet.size() >= 16u)
        {
            dodpcsx2::gif(3, packet.data(), static_cast<uint32_t>(packet.size())); // [DoD] PCSX2's GS gets the raw stream
            emit(packet.data(), static_cast<uint32_t>(packet.size()));
        }
    }
    m_path3MaskedFifo.clear();

    if (m_gifArbiter && drainImmediately)
        m_gifArbiter->drain();
}

void PS2Memory::submitGifPacket(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool drainImmediately, bool path2DirectHl)
{
    if (!data || sizeBytes < 16)
        return;

    if (sizeBytes >= 0x10000u)
    {
        static int s_logged = 0; // [DoD] diagnostics: large GIF packets
        if (s_logged++ < 6)
        {
            uint64_t q[4] = {};
            std::memcpy(q, data, sizeof(q));
            std::fprintf(stderr, "[gif] path%d packet %u bytes: %016llX %016llX | %016llX %016llX pending3=%u\n",
                         static_cast<int>(pathId), sizeBytes, static_cast<unsigned long long>(q[0]),
                         static_cast<unsigned long long>(q[1]), static_cast<unsigned long long>(q[2]),
                         static_cast<unsigned long long>(q[3]), g_dodPath3PendingImageQwc);
        }
    }

    if (pathId == GifPathId::Path3)
    {
        if (m_path3Masked)
        {
            m_path3MaskedFifo.emplace_back(data, data + sizeBytes);
            return;
        }
        flushMaskedPath3Packets(false);
        // [DoD] PCSX2's GS keeps each path's tag state itself: it gets the transfer as it is (before the synthetic
        // continuation tag below, which only the runtime's own GIF parser needs).
        dodpcsx2::gif(3, data, sizeBytes);

        // [DoD] The GIF keeps a tag's state across DMA transfers: games send an IMAGE tag in one PATH3 transfer
        // and its pixels in the next. Continue such an IMAGE behind a synthetic tag instead of parsing the pixels
        // as GIF tags (which wrote junk to FRAME and other registers).
        if (g_dodPath3PendingImageQwc != 0u)
        {
            const uint32_t chunkQw = std::min<uint32_t>(g_dodPath3PendingImageQwc, sizeBytes / 16u);
            if (chunkQw != 0u)
            {
                std::vector<uint8_t> packet(16u + static_cast<size_t>(chunkQw) * 16u, 0u);
                const uint64_t tag = static_cast<uint64_t>(chunkQw & 0x7FFFu) |
                                     ((g_dodPath3PendingImageQwc == chunkQw) ? (1ull << 15) : 0ull) |
                                     (2ull << 58);
                std::memcpy(packet.data(), &tag, sizeof(tag));
                std::memcpy(packet.data() + 16u, data, static_cast<size_t>(chunkQw) * 16u);
                g_dodPath3PendingImageQwc -= chunkQw;
                if (m_gifArbiter)
                    m_gifArbiter->submit(pathId, packet.data(), static_cast<uint32_t>(packet.size()), path2DirectHl);
                else if (m_gifPacketCallback)
                    m_gifPacketCallback(packet.data(), static_cast<uint32_t>(packet.size()));
                if (m_gifArbiter && drainImmediately)
                    m_gifArbiter->drain();
                data += static_cast<size_t>(chunkQw) * 16u;
                sizeBytes -= chunkQw * 16u;
                if (sizeBytes < 16u)
                    return;
            }
        }
        g_dodPath3PendingImageQwc = dodPendingGifImageQwc(data, sizeBytes);
    }
    else
    {
        dodpcsx2::gif(static_cast<int>(pathId), data, sizeBytes); // [DoD] PATH1 / PATH2, raw
    }

    if (m_gifArbiter)
        m_gifArbiter->submit(pathId, data, sizeBytes, path2DirectHl);
    else if (m_gifPacketCallback)
        m_gifPacketCallback(data, sizeBytes);

    if (m_gifArbiter && drainImmediately)
        m_gifArbiter->drain();
}

void PS2Memory::processGIFPacket(uint32_t srcPhysAddr, uint32_t qwCount)
{
    if (!m_rdram || qwCount == 0)
        return;
    const uint64_t bytes64 = static_cast<uint64_t>(qwCount) * 16ull;
    uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
    uint32_t bytesLeft = sizeBytes;
    while (bytesLeft >= 16)
    {
        if (srcPhysAddr >= PS2_RAM_SIZE)
            srcPhysAddr = 0;
        uint32_t chunk = bytesLeft;
        if (srcPhysAddr + chunk > PS2_RAM_SIZE)
            chunk = PS2_RAM_SIZE - srcPhysAddr;
        if (chunk == 0)
            break;

        m_seenGifCopy = true;
        m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
        submitGifPacket(GifPathId::Path3, m_rdram + srcPhysAddr, chunk);

        bytesLeft -= chunk;
        srcPhysAddr += chunk;
    }
}

void PS2Memory::processGIFPacket(const uint8_t *data, uint32_t sizeBytes)
{
    if (m_gifArbiter)
        submitGifPacket(GifPathId::Path3, data, sizeBytes);
    else if (m_gifPacketCallback && data && sizeBytes >= 16)
        m_gifPacketCallback(data, sizeBytes);
}

bool PS2Memory::tryProcessNativeGifImageUploadChain(GS &gs, uint32_t tadr, uint32_t chcr)
{
    static constexpr uint32_t GIF_CHANNEL = 0x1000A000u;
    static constexpr uint32_t D_STAT = 0x1000E010u;
    static constexpr uint32_t D_CTRL = 0x1000E000u;

    if (dodpcsx2::active())
        return false; // [DoD] the shortcut skips submitGifPacket, where PCSX2's GS takes the stream

    if (!m_rdram || !m_gsVRAM || m_path3Masked)
        return false;
    if (m_gifArbiter && !m_gifArbiter->empty())
        return false;
    if ((chcr & 0x100u) == 0u || ((chcr >> 2u) & 0x3u) != 1u)
        return false;
    if ((chcr & (1u << 7u)) != 0u || ((chcr >> 4u) & 0x3u) != 0u)
        return false;

    const auto dctrlIt = m_ioRegisters.find(D_CTRL);
    if (dctrlIt != m_ioRegisters.end() && ((dctrlIt->second & 0x1u) == 0u))
        return false;

    auto resolveContiguous = [&](uint32_t guestAddr, uint32_t bytes, const uint8_t *&out) -> bool
    {
        try
        {
            const bool scratch = isScratchpad(guestAddr);
            const uint32_t phys = translateAddress(guestAddr);
            const uint8_t *base = scratch ? m_scratchpad : m_rdram;
            const uint32_t limit = scratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
            if (!base || phys > limit || bytes > limit - phys)
                return false;
            out = base + phys;
            return true;
        }
        catch (const std::exception &)
        {
            return false;
        }
    };

    auto loadDmaTagAt = [&](uint32_t guestAddr, DmaTagView &out) -> bool
    {
        const uint8_t *ptr = nullptr;
        if (!resolveContiguous(guestAddr, 16u, ptr))
            return false;
        out = decodeDmaTag(loadScalar<uint64_t>(ptr, 0u, 16u, "native gif dma tag", guestAddr));
        return true;
    };

    auto decodeSetupPayload = [&](const uint8_t *payload, uint64_t (&regs)[4]) -> bool
    {
        const uint64_t tagLo = loadScalar<uint64_t>(payload, 0u, 80u, "native gif setup tag", 0u);
        const uint64_t tagHi = loadScalar<uint64_t>(payload, 8u, 80u, "native gif setup regs", 0u);
        if (gifTagNloop(tagLo) != 4u ||
            gifTagFlg(tagLo) != GIF_FMT_PACKED ||
            gifTagNreg(tagLo) != 1u ||
            (tagHi & 0xFull) != 0x0Eull)
        {
            return false;
        }

        static constexpr uint8_t kExpectedRegs[4] = {
            GS_REG_BITBLTBUF,
            GS_REG_TRXPOS,
            GS_REG_TRXREG,
            GS_REG_TRXDIR,
        };

        uint32_t offset = 16u;
        for (uint32_t i = 0; i < 4u; ++i)
        {
            regs[i] = loadScalar<uint64_t>(payload, offset, 80u, "native gif setup value", 0u);
            const uint64_t reg = loadScalar<uint64_t>(payload, offset + 8u, 80u, "native gif setup register", 0u);
            if ((reg & 0xFFu) != kExpectedRegs[i])
                return false;
            offset += 16u;
        }

        const uint32_t trxdirMode = static_cast<uint32_t>(regs[3] & 0x3ull);
        const uint32_t rrw = static_cast<uint32_t>(regs[2] & 0xFFFull);
        const uint32_t rrh = static_cast<uint32_t>((regs[2] >> 32u) & 0xFFFull);
        return trxdirMode == 0u && rrw != 0u && rrh != 0u;
    };

    DmaTagView setupTag{};
    if (!loadDmaTagAt(tadr, setupTag) ||
        setupTag.id != 1u ||
        setupTag.qwc != 5u ||
        setupTag.irq)
    {
        return false;
    }

    const uint8_t *setupPayload = nullptr;
    const uint32_t setupPayloadAddr = tadr + 16u;
    if (!resolveContiguous(setupPayloadAddr, 5u * 16u, setupPayload))
        return false;

    uint64_t setupRegs[4] = {};
    if (!decodeSetupPayload(setupPayload, setupRegs))
        return false;

    uint32_t imageTagDmaAddr = setupPayloadAddr + 5u * 16u;
    DmaTagView imageTagDma{};
    if (!loadDmaTagAt(imageTagDmaAddr, imageTagDma) ||
        imageTagDma.id != 1u ||
        imageTagDma.qwc != 1u ||
        imageTagDma.irq)
    {
        return false;
    }

    const uint8_t *imageGifTag = nullptr;
    if (!resolveContiguous(imageTagDmaAddr + 16u, 16u, imageGifTag))
        return false;

    const uint64_t imageTagLo = loadScalar<uint64_t>(imageGifTag, 0u, 16u, "native gif image tag", imageTagDmaAddr + 16u);
    if (gifTagFlg(imageTagLo) != GIF_FMT_IMAGE)
        return false;

    const uint32_t imageQwc = gifTagNloop(imageTagLo);
    if (imageQwc == 0u)
        return false;

    const uint64_t imageBytes64 = static_cast<uint64_t>(imageQwc) * 16ull;
    if (imageBytes64 > 0xFFFFFFFFull)
        return false;
    const uint32_t imageBytes = static_cast<uint32_t>(imageBytes64);

    const uint32_t payloadTagAddr = imageTagDmaAddr + 32u;
    DmaTagView payloadTag{};
    if (!loadDmaTagAt(payloadTagAddr, payloadTag) ||
        payloadTag.qwc != imageQwc ||
        payloadTag.irq)
    {
        return false;
    }

    uint32_t imageDataAddr = 0u;
    uint32_t finalTadr = payloadTagAddr;
    uint32_t lastTagUpper = payloadTag.upper;
    if (payloadTag.id == 3u || payloadTag.id == 4u)
    {
        imageDataAddr = payloadTag.addr;
        const uint32_t terminalTagAddr = payloadTagAddr + 16u;
        DmaTagView terminalTag{};
        if (!loadDmaTagAt(terminalTagAddr, terminalTag) ||
            terminalTag.qwc != 0u ||
            terminalTag.irq ||
            (terminalTag.id != 0u && terminalTag.id != 7u))
        {
            return false;
        }
        finalTadr = (terminalTag.id == 0u) ? (terminalTagAddr + 16u) : terminalTagAddr;
        lastTagUpper = terminalTag.upper;
    }
    else if (payloadTag.id == 7u)
    {
        imageDataAddr = payloadTagAddr + 16u;
        finalTadr = payloadTagAddr;
    }
    else
    {
        return false;
    }

    const uint8_t *imageData = nullptr;
    if (!resolveContiguous(imageDataAddr, imageBytes, imageData))
        return false;

    m_dmaStartCount.fetch_add(1, std::memory_order_relaxed);
    m_seenGifCopy = true;
    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
    gs.uploadImageNative(setupRegs[0], setupRegs[1], setupRegs[2], setupRegs[3], imageData, imageBytes);

    m_ioRegisters[GIF_CHANNEL + 0x30u] = finalTadr;
    m_ioRegisters[GIF_CHANNEL + 0x40u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x50u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x00u] = ((chcr & 0x0000FFFFu) | (lastTagUpper << 16u)) & ~0x100u;
    m_ioRegisters[GIF_CHANNEL + 0x20u] = 0u;

    uint32_t dstat = m_ioRegisters.count(D_STAT) ? m_ioRegisters[D_STAT] : 0u;
    dstat |= (1u << 2u);
    const uint32_t status = dstat & 0x3FFu;
    const uint32_t mask = (dstat >> 16u) & 0x3FFu;
    if ((status & mask) != 0u)
        dstat |= (1u << 31u);
    else
        dstat &= ~(1u << 31u);
    m_ioRegisters[D_STAT] = dstat;
    queueCompletedDmacCause(2u);
    return true;
}

bool PS2Memory::tryProcessNativeGifPackedChain(GS &gs, uint32_t tadr, uint32_t chcr)
{
    static constexpr uint32_t GIF_CHANNEL = 0x1000A000u;
    static constexpr uint32_t D_STAT = 0x1000E010u;
    static constexpr uint32_t D_CTRL = 0x1000E000u;

    if (dodpcsx2::active())
        return false; // [DoD] the shortcut skips submitGifPacket, where PCSX2's GS takes the stream

    if (!m_rdram || !m_gsVRAM || m_path3Masked)
        return false;
    if (m_gifArbiter && !m_gifArbiter->empty())
        return false;
    if ((chcr & 0x100u) == 0u || ((chcr >> 2u) & 0x3u) != 1u)
        return false;
    if ((chcr & (1u << 7u)) != 0u || ((chcr >> 4u) & 0x3u) != 0u)
        return false;

    const auto dctrlIt = m_ioRegisters.find(D_CTRL);
    if (dctrlIt != m_ioRegisters.end() && ((dctrlIt->second & 0x1u) == 0u))
        return false;

    auto resolveContiguous = [&](uint32_t guestAddr, uint32_t bytes, const uint8_t *&out) -> bool
    {
        try
        {
            const bool scratch = isScratchpad(guestAddr);
            const uint32_t phys = translateAddress(guestAddr);
            const uint8_t *base = scratch ? m_scratchpad : m_rdram;
            const uint32_t limit = scratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
            if (!base || phys > limit || bytes > limit - phys)
                return false;
            out = base + phys;
            return true;
        }
        catch (const std::exception &)
        {
            return false;
        }
    };

    const uint8_t *tagPtr = nullptr;
    if (!resolveContiguous(tadr, 16u, tagPtr))
        return false;

    const DmaTagView tag = decodeDmaTag(loadScalar<uint64_t>(tagPtr, 0u, 16u, "native packed gif dma tag", tadr));
    if (tag.id != 7u || tag.qwc == 0u || tag.irq)
        return false;

    const uint64_t payloadBytes64 = static_cast<uint64_t>(tag.qwc) * 16ull;
    if (payloadBytes64 > 0xFFFFFFFFull)
        return false;
    const uint32_t payloadBytes = static_cast<uint32_t>(payloadBytes64);

    const uint8_t *payload = nullptr;
    if (!resolveContiguous(tadr + 16u, payloadBytes, payload))
        return false;
    if (!gs.processNativePackedGIFPacket(payload, payloadBytes))
        return false;

    m_dmaStartCount.fetch_add(1, std::memory_order_relaxed);
    m_seenGifCopy = true;
    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);

    m_ioRegisters[GIF_CHANNEL + 0x30u] = tadr;
    m_ioRegisters[GIF_CHANNEL + 0x40u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x50u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x00u] = ((chcr & 0x0000FFFFu) | (tag.upper << 16u)) & ~0x100u;
    m_ioRegisters[GIF_CHANNEL + 0x20u] = 0u;

    uint32_t dstat = m_ioRegisters.count(D_STAT) ? m_ioRegisters[D_STAT] : 0u;
    dstat |= (1u << 2u);
    const uint32_t status = dstat & 0x3FFu;
    const uint32_t mask = (dstat >> 16u) & 0x3FFu;
    if ((status & mask) != 0u)
        dstat |= (1u << 31u);
    else
        dstat &= ~(1u << 31u);
    m_ioRegisters[D_STAT] = dstat;
    queueCompletedDmacCause(2u);
    return true;
}

int PS2Memory::pollDmaRegisters()
{
    return 0;
}

uint32_t PS2Memory::readIORegister(uint32_t address)
{
    if (dodio::census()) // [DoD] DOD_IO_CENSUS: note the value this call returns
    {
        static thread_local bool inside = false;
        if (!inside)
        {
            inside = true;
            const uint32_t value = readIORegister(address);
            inside = false;
            dodio::note(address, false, value);
            return value;
        }
    }
    // EE-side DMA polling can observe capacity released by MPEG take(). This
    // only copies/commits a queued transaction; it cannot enter MPEG/GS code.
    if ((address >= 0x1000B400u && address <= 0x1000B450u) || address == 0x1000E010u)
        if (dodServiceMpegToIpu(m_ioRegisters)) completeDmacChannel(0x1000B400u, 4u);
    if (address == 0x1000B400u && g_dodMpegToIpu.active())
        return m_ioRegisters[address]; // Do not let the legacy generic DMA read clear a pending STR.
    size_t timerIndex = 0u;
    uint32_t timerOffset = 0u;
    if (decodeEeTimerRegister(address, timerIndex, timerOffset))
    {
        dodSettleEeTimersNow(*this);
        const EeTimer &timer = m_eeTimers[timerIndex];
        switch (timerOffset)
        {
        case kEeTimerCountOffset:
            return timer.count & 0xFFFFu;
        case kEeTimerModeOffset:
            return timer.mode & (kEeTimerModeConfigMask | kEeTimerModeStatusMask);
        case kEeTimerCompareOffset:
            return timer.compare & 0xFFFFu;
        case kEeTimerHoldOffset:
            return timer.hold & 0xFFFFu;
        default:
            return 0u;
        }
    }

    if (isGsPrivReg(address))
    {
        // NB: unreachable from read8/16/32/64 today, same reasoning as the write
        // path above; kept correct for direct callers.
        const uint32_t off = address & 7u;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            return static_cast<uint32_t>((gs_regs.csr.load() >> (off * 8u)) & 0xFFFFFFFFull);
        }
        if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            return static_cast<uint32_t>((*reg >> (off * 8u)) & 0xFFFFFFFFull);
        }
        return 0u;
    }

    if (address >= 0x10002000 && address < 0x10002040)
    {
        if (dodac5ipu::enabled())
        {
            dodac5ipu::attach(*this);
            return ps2_ipu_read32(address);
        }
        return ps2_ipu::read32(address - 0x10002000u); // [DoD] IPU registers (ps2_ipu.cpp)
    }

    if (address == 0x10003020u) // GIF_STAT
    {
        dodSettleEeTimersNow(*this); // [DoD] FQC expires when cycles pass (advanceEeTimers)
        uint32_t stat = m_ioRegisters.count(address) ? m_ioRegisters[address] : 0u;
        const uint32_t mode = m_ioRegisters.count(0x10003010u) ? m_ioRegisters[0x10003010u] : 0u;
        const uint32_t ctrl = m_ioRegisters.count(0x10003000u) ? m_ioRegisters[0x10003000u] : 0u;

        // M3R and IMT mirror GIF_MODE, PSE mirrors GIF_CTRL, and M3P is the
        // effective PATH3 mask controlled by the VIF1 MSKPATH3 command.
        stat = (stat & ~0xFu) |
               (mode & 0x1u) |
               (m_path3Masked ? 0x2u : 0u) |
               (mode & 0x4u) |
               (ctrl & 0x8u);
        return stat;
    }

    if (address >= 0x10000000 && address < 0x10010000)
    {
        if (address >= 0x10008000 && address < 0x1000F000)
        {
            if ((address & 0xFF) == 0x00)
            {
                // [DoD] The IPU channels run as the IPU consumes/produces data (AC5 IPU pulls toIPU on demand and
                // fromIPU drains per command): their STR stays set until the transfer really ends. Reading CHCR
                // used to clear it, which stopped a toIPU chain after its first quadwords (CAPCOM logo hung).
                if ((address == 0x1000B400u || address == 0x1000B000u) && dodac5ipu::enabled())
                    return m_ioRegisters[address];
                uint32_t channelStatus = m_ioRegisters[address] & ~0x100u;
                m_ioRegisters[address] = channelStatus;
                return channelStatus;
            }
        }

        if (address >= 0x10000200 && address < 0x10000300)
        {
            return 0;
        }

        if (address >= 0x1000F200 && address <= 0x1000F260)
        {
            if (address == 0x1000F230)
            {
                return 0x60000;
            }
            if (address == 0x1000F240)
            {
                return 0xF0000002;
            }
            return 0;
        }
    }

    auto it = m_ioRegisters.find(address);
    if (it != m_ioRegisters.end())
    {
        return it->second;
    }

    return 0;
}

void PS2Memory::registerCodeRegion(uint32_t start, uint32_t end)
{
    if (end <= start)
    {
        std::cerr << "Ignoring invalid code region: start=0x" << std::hex << start
                  << " end=0x" << end << std::dec << std::endl;
        return;
    }

    if ((end - start) > PS2_RAM_SIZE)
    {
        std::cerr << "Ignoring oversized code region: start=0x" << std::hex << start
                  << " end=0x" << end << std::dec << std::endl;
        return;
    }

    for (const auto &existing : m_codeRegions)
    {
        if (existing.start == start && existing.end == end)
        {
            return;
        }
    }

    CodeRegion region;
    region.start = start;
    region.end = end;

    size_t sizeInWords = (end - start + 3u) / 4u;
    region.modified.resize(sizeInWords, false);

    m_codeRegions.push_back(region);
    RUNTIME_LOG("Registered code region: " << std::hex << start << " - " << end << std::dec);
}

bool PS2Memory::isAddressInRegion(uint32_t address, const CodeRegion &region)
{
    return (address >= region.start && address < region.end);
}

bool PS2Memory::isCodeAddress(uint32_t address) const
{
    for (const auto &region : m_codeRegions)
    {
        if (address >= region.start && address < region.end)
        {
            return true;
        }
    }
    return false;
}

void PS2Memory::markModified(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size())
            {
                region.modified[bitIndex] = true;
                RUNTIME_LOG("Marked code at " << std::hex << addr << std::dec << " as modified");
            }
        }
    }
}

bool PS2Memory::isCodeModified(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return false;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (const auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size() && region.modified[bitIndex])
            {
                return true; // Found modified code
            }
        }
    }

    return false; // No modifications found
}

void PS2Memory::clearModifiedFlag(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size())
            {
                region.modified[bitIndex] = false;
            }
        }
    }
}
