#include "iop_spu2.h"
#include "iop_spu2_gauss.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace ps2x::iop
{
    void (*g_dodSpu2StemsSink)(const int16_t *stems6) = nullptr;
    void (*g_dodSpu2EventSink)(const char *text) = nullptr;
    uint32_t *g_dodAdmaLastWrite = nullptr;
    uint32_t g_dodAdmaNow = 1u;
    uint32_t *g_dodAdmaLastPc = nullptr;
    uint32_t *g_dodAdmaLastSrc = nullptr;
    uint32_t g_dodIopStorePc = 0u;
    bool g_dodAdmaCopyActive = false;
    uint32_t g_dodAdmaCopyDelta = 0u;

    uint32_t g_dodAdmaLoadPcLo = 0u;
    uint32_t g_dodAdmaLoadPcHi = 0u;
    DodSchedEvent g_dodSchedRing[256]{};
    uint32_t g_dodSchedCount = 0u;
    void (*g_dodDescribeIopThreads)() = nullptr;

    namespace
    {
        struct IopIoEntry
        {
            uint64_t reads = 0u, writes = 0u;
            uint32_t orBits = 0u, andBits = ~0u, lastWrite = 0u, lastRead = 0u;
        };
        struct IopIoCensus
        {
            std::mutex mutex;
            std::map<uint32_t, IopIoEntry> entries;
            std::string path;
            std::chrono::steady_clock::time_point lastDump = std::chrono::steady_clock::now();
            uint32_t sinceCheck = 0u;
            void dump()
            {
                std::FILE *f = std::fopen(path.c_str(), "w");
                if (!f)
                    return;
                std::fprintf(f, "address,reads,writes,or_bits,and_bits,last_write,last_read\n");
                for (const auto &[addr, e] : entries)
                    std::fprintf(f, "%08X,%llu,%llu,%08X,%08X,%08X,%08X\n", addr, static_cast<unsigned long long>(e.reads),
                                 static_cast<unsigned long long>(e.writes), e.orBits, e.writes ? e.andBits : 0u, e.lastWrite, e.lastRead);
                std::fclose(f);
            }
            ~IopIoCensus()
            {
                std::lock_guard<std::mutex> lock(mutex);
                dump();
            }
        };
        IopIoCensus *iopIoCensus()
        {
            static IopIoCensus *const c = []() -> IopIoCensus * {
                const char *v = std::getenv("DOD_IO_CENSUS");
                if (!v || !*v)
                    return nullptr;
                static IopIoCensus instance;
                instance.path = std::string(v) + ".iop.csv";
                return &instance;
            }();
            return c;
        }
    }

    void dodIopIoNote(uint32_t address, bool write, uint32_t value)
    {
        IopIoCensus *c = iopIoCensus();
        if (!c)
            return;
        std::lock_guard<std::mutex> lock(c->mutex);
        IopIoEntry &e = c->entries[address];
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

    namespace
    {
        uint32_t *g_dodAdmaLastRead = nullptr;

        // Consecutive stale halfwords read by the mixer, printed as one line.
        struct StaleRun
        {
            uint32_t first = 0u, next = 0u, count = 0u, pc = 0u, writer = 0u, writtenAgo = 0u, readAgo = 0u, at = 0u;
        };
        void flushStaleRun(StaleRun &run)
        {
            static uint32_t lines = 0u;
            if (run.count >= 16u && lines < 1500u) // shorter runs are mixer state words read every pass
            {
                ++lines;
                std::fprintf(stderr, "[adma-load] stale run sample %u pc %05X addr %05X..%05X (%u halfwords) written %u ago by %05X, last read %u ago\n",
                             run.at, run.pc, run.first, run.next - 2u, run.count, run.writtenAgo, run.writer, run.readAgo);
            }
            run.count = 0u;
        }
    }

    void dodAdmaNoteLoad(uint32_t pc, uint32_t address, uint32_t size)
    {
        if (!g_dodAdmaLastWrite)
            return;
        if (!g_dodAdmaLastRead)
            g_dodAdmaLastRead = new uint32_t[0x200000u / 2u]();
        static StaleRun run;
        struct Counter
        {
            uint32_t staleBlock = 0u, staleCpu = 0u, firstAddr = 0u, lastAddr = 0u, writerPc = 0u;
        };
        static std::map<uint32_t, Counter> counters; // by load pc
        static uint32_t nextReport = 5u * 48000u, detailLines = 0u;
        const uint32_t now = g_dodAdmaNow;
        const uint32_t phys = address & 0x1FFFFFu;
        for (uint32_t h = phys >> 1u; h <= (phys + size - 1u) >> 1u && h < 0x100000u; ++h)
        {
            const uint32_t written = g_dodAdmaLastWrite[h] & 0x7FFFFFFFu;
            const uint32_t read = g_dodAdmaLastRead[h];
            // Stale: written once, read since, not rewritten, and that read was a while ago (not the same pass).
            if (written != 0u && read != 0u && written <= read && now > read + 64u)
            {
                Counter &c = counters[pc];
                const bool block = (g_dodAdmaLastWrite[h] & 0x80000000u) != 0u;
                (block ? c.staleBlock : c.staleCpu)++;
                if (c.firstAddr == 0u || (h << 1u) < c.firstAddr)
                    c.firstAddr = h << 1u;
                c.lastAddr = std::max(c.lastAddr, h << 1u);
                c.writerPc = g_dodAdmaLastPc[h];
                if (run.count != 0u && (h << 1u) == run.next)
                {
                    ++run.count;
                    run.next += 2u;
                }
                else
                {
                    flushStaleRun(run);
                    run = StaleRun{h << 1u, (h << 1u) + 2u, 1u, pc, g_dodAdmaLastPc[h], now - written, now - read, now};
                }
            }
            else if (run.count != 0u && (h << 1u) != run.next - 2u)
                flushStaleRun(run);
            (void)detailLines;
            g_dodAdmaLastRead[h] = now;
        }
        if (now >= nextReport)
        {
            nextReport = now + 5u * 48000u;
            std::fprintf(stderr, "[adma-load] %.1f s stale reads by load pc:", now / 48000.0);
            for (const auto &[loadPc, c] : counters)
                if (c.staleBlock + c.staleCpu >= 8u)
                    std::fprintf(stderr, " %05X: block %u cpu %u [%05X..%05X] writer %05X;", loadPc, c.staleBlock, c.staleCpu, c.firstAddr,
                                 c.lastAddr, c.writerPc);
            std::fprintf(stderr, "\n");
            counters.clear();
        }
    }
}

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kAddressMask = IopSpu2::RamHalfwords - 1u;
        constexpr uint32_t kCoreStride = 0x400u;
        constexpr uint32_t kVoiceParamEnd = 0x180u;
        constexpr uint32_t kVoiceAddressBase = 0x1C0u;
        constexpr uint32_t kVoiceAddressEnd = kVoiceAddressBase + 24u * 0xCu;
        constexpr uint32_t kMasterBase = 0x760u;
        constexpr uint32_t kMasterStride = 0x28u;
        constexpr uint32_t kSpdifIrqInfo = 0x7C2u;

        constexpr int32_t kAdpcmPos[5] = {0, 60, 115, 98, 122};
        constexpr int32_t kAdpcmNeg[5] = {0, 0, -52, -55, -60};

        // [DoD] Reverb registers (PCSX2 RegTable.cpp): core offsets 0x2E0/0x2E2 ESA, 0x2E4..0x33A the 22 address
        // offsets (high, low halfword each; order of RevbAddr), 0x33C EEA (high halfword; 0x33E is not a register);
        // master area +0x14..+0x26 the volumes (order of RevbVol).
        constexpr uint32_t kRevbEsa = 0x2E0u;
        constexpr uint32_t kRevbAddrBase = 0x2E4u;
        constexpr uint32_t kRevbEea = 0x33Cu;
        constexpr uint32_t kRevbVolBase = 0x14u;
        enum RevbAddr : uint32_t
        {
            APF1_SIZE, APF2_SIZE, SAME_L_DST, SAME_R_DST, COMB1_L_SRC, COMB1_R_SRC, COMB2_L_SRC, COMB2_R_SRC,
            SAME_L_SRC, SAME_R_SRC, DIFF_L_DST, DIFF_R_DST, COMB3_L_SRC, COMB3_R_SRC, COMB4_L_SRC, COMB4_R_SRC,
            DIFF_L_SRC, DIFF_R_SRC, APF1_L_DST, APF1_R_DST, APF2_L_DST, APF2_R_DST,
        };
        enum RevbVol : uint32_t
        {
            IIR_VOL, COMB1_VOL, COMB2_VOL, COMB3_VOL, COMB4_VOL, WALL_VOL, APF1_VOL, APF2_VOL, IN_COEF_L, IN_COEF_R,
        };

        // PCSX2 ReverbResample.cpp: the 39-tap filter that takes the reverb input down to half rate (one channel per
        // sample) and its output back up (coefficients doubled).
        constexpr std::array<int16_t, 39> kRevbDownCoefs = {
            -1, 0, 2, 0, -10, 0, 35, 0, -103, 0, 266, 0, -616, 0, 1332, 0, -2960, 0, 10246, 16384,
            10246, 0, -2960, 0, 1332, 0, -616, 0, 266, 0, -103, 0, 35, 0, -10, 0, 2, 0, -1};
        constexpr std::array<int16_t, 39> kRevbUpCoefs = []()
        {
            std::array<int16_t, 39> up{};
            for (size_t i = 0; i < up.size(); ++i)
                up[i] = static_cast<int16_t>(std::clamp<int32_t>(kRevbDownCoefs[i] * 2, -32768, 32767));
            return up;
        }();

        // [DoD] DOD_SPU2_REVERB=0: no reverb (the mix of build 276 and before, for A/B); on by default.
        bool reverbOn()
        {
            static const bool on = []() { const char *v = std::getenv("DOD_SPU2_REVERB"); return !(v && v[0] == '0'); }();
            return on;
        }

        // [DoD] DOD_SPU2_REVERB_STEMS=<file>: raw 4-channel 16-bit 48 kHz: core 1 reverb input (the wet sum,
        // saturated) L/R and core 1 reverb return after EVOL L/R.
        std::FILE *reverbStemsFile()
        {
            static std::FILE *const file = []() -> std::FILE * {
                const char *path = std::getenv("DOD_SPU2_REVERB_STEMS");
                return path && *path ? std::fopen(path, "wb") : nullptr;
            }();
            return file;
        }

        // PCSX2's MUL(x, y) = x * y >> 15 on 32-bit int (wraps like x86 imul if it ever overflows).
        int32_t revbMul(int32_t x, int32_t y)
        {
            return static_cast<int32_t>(static_cast<uint32_t>(x) * static_cast<uint32_t>(y)) >> 15;
        }

        // [DoD] DOD_SPU2_TRACE=1: the first AutoDMA and routing events on stderr.
        bool traceOn()
        {
            static const bool on = []() { const char *v = std::getenv("DOD_SPU2_TRACE"); return v && v[0] == '1'; }();
            return on;
        }

        void trace(const char *format, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
        {
            static int count = 0;
            if (!traceOn() || count >= 400)
                return;
            ++count;
            std::fprintf(stderr, "[spu2-trace] ");
            std::fprintf(stderr, format, a, b, c, d);
            std::fprintf(stderr, "\n");
        }

        // AutoDMA underruns (a gap of silence in the streamed sound) are counted apart from the other events, so a long
        // run still reports every one of them (up to 2000).
        void traceUnderrun(uint32_t core, uint32_t filled, uint32_t inputPos, uint32_t sample)
        {
            if (g_dodSpu2EventSink)
            {
                char text[160];
                std::snprintf(text, sizeof(text), "SPU2 core %u AutoDMA underrun: %u of 1024 bytes (silence in the stream), SPU2 sample %u",
                              core, filled, sample);
                g_dodSpu2EventSink(text);
            }
            static uint32_t count = 0;
            if (!traceOn() || count >= 2000u)
                return;
            ++count;
            std::fprintf(stderr, "[spu2-underrun] #%u core %u AutoDMA underrun: pulled %u of 1024 bytes at inputPos %03X, sample %u (%.2f s)\n",
                         count, core, filled, inputPos, sample, sample / 48000.0);
        }

        // [DoD] DOD_SPU2_STEMS=<file>: raw 6-channel 16-bit 48 kHz stems (see generate).
        std::FILE *stemsFile()
        {
            static std::FILE *const file = []() -> std::FILE * {
                const char *path = std::getenv("DOD_SPU2_STEMS");
                return path && *path ? std::fopen(path, "wb") : nullptr;
            }();
            return file;
        }

        int32_t clamp16(int32_t value)
        {
            return std::clamp(value, -32768, 32767);
        }

        // PCSX2 ReverbDownsample / ReverbUpsample (the reference versions; PCSX2 runs SIMD versions of the same
        // filter, which round each tap): the 39 samples before `pos` of a doubled 64-sample ring.
        int32_t reverbFir(const std::array<int16_t, 128> &buffer, uint32_t pos, const std::array<int16_t, 39> &coefs)
        {
            const uint32_t index = (pos - 39u) & 63u;
            int64_t sum = 0;
            for (uint32_t i = 0; i < 39u; ++i)
                sum += static_cast<int32_t>(buffer[index + i]) * coefs[i];
            return clamp16(static_cast<int32_t>(sum >> 15));
        }

        // [DoD] DOD_ADMA_WATCH=1: see g_dodAdmaLastWrite. lastPull is the SPU2 sample (+1) at which each IOP RAM
        // halfword was last pulled by AutoDMA. Bit 31 of a last-write entry marks a block write (writeRam / zeroRam:
        // HLE memcpy, SIF, loader) rather than a CPU store.
        uint32_t *admaLastPull()
        {
            static uint32_t *const table = []() -> uint32_t * {
                const char *v = std::getenv("DOD_ADMA_WATCH");
                if (!v || v[0] != '1')
                    return nullptr;
                constexpr size_t halfwords = 0x200000u / 2u;
                g_dodAdmaLastWrite = new uint32_t[halfwords]();
                g_dodAdmaLastPc = new uint32_t[halfwords]();
                g_dodAdmaLastSrc = new uint32_t[halfwords]();
                // Load checks only with DOD_ADMA_WATCH_PC (e.g. BE000:BF130 = CRI_ADXI's mixer at base 0xBA400): they
                // slow the IOP enough that the right-channel fault stopped showing up.
                if (const char *range = std::getenv("DOD_ADMA_WATCH_PC"))
                    (void)std::sscanf(range, "%x:%x", &g_dodAdmaLoadPcLo, &g_dodAdmaLoadPcHi);
                return new uint32_t[halfwords]();
            }();
            return table;
        }

        struct AdmaWatchStats
        {
            uint32_t pulls = 0u, staleL = 0u, staleR = 0u, staleHwL = 0u, staleHwR = 0u, lines = 0u, nextSummary = 48000u;
        };

        uint32_t setLow16(uint32_t current, uint16_t value)
        {
            return (current & 0xFFFF0000u) | value;
        }

        uint32_t setHigh8(uint32_t current, uint16_t value)
        {
            return (current & 0xFFFFu) | (static_cast<uint32_t>(value & 0xFFu) << 16u);
        }

        // SPU2 addresses are 20-bit halfword addresses split into a high word (bits 16-19) and a low word.
        uint32_t setAddressHigh(uint32_t current, uint16_t value)
        {
            return ((static_cast<uint32_t>(value & 0xFu) << 16u) | (current & 0xFFFFu)) & kAddressMask;
        }

        uint32_t setAddressLow(uint32_t current, uint16_t value)
        {
            return ((current & 0xF0000u) | value) & kAddressMask;
        }
    }

    void IopSpu2::Envelope::reset(uint8_t newRate, bool newDecreasing, bool newExponential)
    {
        rate = newRate;
        decreasing = newDecreasing;
        exponential = newExponential;
        counter = 0u;
        increment = 0x8000u;
        const int32_t baseStep = 7 - static_cast<int32_t>(rate & 3u);
        step = decreasing ? ~baseStep : baseStep;
        if (rate < 44u)
            step <<= (11 - (rate >> 2u));
        else if (rate >= 48u)
            increment >>= ((rate >> 2u) - 11u);
    }

    // [DoD] As PCSX2's V_ADSR::Calculate: an exponential increase above 0x6000 steps 4 times less often (it used to
    // shrink the step instead for slow rates and started at 0x6000), and the counter always advances by at least 1
    // (the slowest rates never moved).
    int32_t IopSpu2::Envelope::tick(int32_t level)
    {
        uint32_t thisIncrement = increment;
        int32_t thisStep = step;
        if (exponential)
        {
            if (!decreasing && level > 0x6000)
                thisIncrement >>= 2u;
            if (decreasing)
                thisStep = static_cast<int16_t>((thisStep * level) >> 15);
        }
        thisIncrement = std::max<uint32_t>(1u, thisIncrement);
        counter += thisIncrement;
        if (counter < 0x8000u)
            return level;
        counter = 0u;
        return std::clamp(level + thisStep, 0, 0x7FFF);
    }

    void IopSpu2::Volume::write(uint16_t value)
    {
        reg = value;
        if ((value & 0x8000u) == 0u)
            current = static_cast<int16_t>(static_cast<uint16_t>(value << 1u));
    }

    // [DoD] PCSX2 V_VolumeSlide::Update: bits 0-1 step, 2-6 shift, 12 negative phase, 13 decrease, 14 exponential.
    void IopSpu2::Volume::tick()
    {
        if ((reg & 0x8000u) == 0u)
            return;
        const int32_t stepBits = reg & 3u;
        const int32_t shift = (reg >> 2u) & 0x1Fu;
        const bool phase = (reg & 0x1000u) != 0u;
        const bool decr = (reg & 0x2000u) != 0u;
        const bool exp = (reg & 0x4000u) != 0u;
        int32_t stepSize = 7 - stepBits;
        if (decr)
            stepSize = ~stepSize;
        uint32_t counterInc = 0x8000u >> std::max(0, shift - 11);
        int32_t levelInc = stepSize << std::max(0, 11 - shift);
        if (exp)
        {
            if (!decr && current > 0x6000)
                counterInc >>= 2u;
            if (decr)
                levelInc = static_cast<int16_t>((levelInc * current) >> 15);
        }
        if (stepBits != 3 && shift != 0x1F)
            counterInc = std::max<uint32_t>(1u, counterInc);
        counter += counterInc;
        if (!(exp && decr))
            levelInc = phase ? -levelInc : levelInc;
        if (counter >= 0x8000u)
        {
            counter = 0u;
            if (!decr)
                current = std::clamp(current + levelInc, -32768, 32767);
            else
            {
                int32_t low = phase ? -32768 : 0;
                int32_t high = phase ? 0 : 32767;
                if (exp)
                {
                    low = 0;
                    high = 32767;
                }
                current = std::clamp(current + levelInc, low, high);
            }
        }
    }

    IopSpu2::IopSpu2()
        : m_ram(RamHalfwords)
    {
        (void)admaLastPull(); // [DoD] DOD_ADMA_WATCH: allocate the tables before the first IOP write
        reset();
    }

    void IopSpu2::reset()
    {
        std::fill(m_ram.begin(), m_ram.end(), uint16_t{0});
        m_regs.fill(0u);
        m_cores = {};
        // Some sound drivers never write the master and input volumes; on hardware the BIOS
        // leaves them at full scale before the game boots.
        for (Core &core : m_cores)
        {
            core.mvolL.write(0x3FFFu);
            core.mvolR.write(0x3FFFu);
            core.avolL = core.avolR = 0x7FFF;
            core.bvolL = core.bvolR = 0x7FFF;
        }
        // [DoD] Effects area at reset (PCSX2 V_Core::Init).
        m_cores[0].esa = 0xEFFF8u;
        m_cores[0].eea = 0xEFFFFu;
        m_cores[1].esa = 0xFFFF8u;
        m_cores[1].eea = 0xFFFFFu;
        m_irqPending = false;
    }

    bool IopSpu2::takeIrq() noexcept
    {
        const bool pending = m_irqPending;
        m_irqPending = false;
        return pending;
    }

    void IopSpu2::checkIrq(uint32_t begin, uint32_t count)
    {
        for (int c = 0; c < 2; ++c)
        {
            Core &core = m_cores[c];
            if ((core.attr & 0x40u) == 0u)
                continue;
            if (((core.irqa - begin) & kAddressMask) < count)
            {
                m_regs[kSpdifIrqInfo / 2u] |= static_cast<uint16_t>(4u << c);
                m_irqPending = true;
            }
        }
    }

    uint16_t IopSpu2::read16(uint32_t offset) const
    {
        const uint16_t value = read16Raw(offset);
        dodIopIoNote(0x1F900000u + (offset & (RegisterBytes - 2u)), false, value); // [DoD] DOD_IO_CENSUS
        return value;
    }

    uint16_t IopSpu2::read16Raw(uint32_t offset) const
    {
        offset &= (RegisterBytes - 1u) & ~1u;
        // [DoD] The master area (0x760..0x7AF) lies inside core 1's 0x400..0x7FF block, so it is checked first (it
        // came after the core block and was never reached: MVOLX read back the stored value).
        if (offset >= kMasterBase && offset < kMasterBase + 2u * kMasterStride)
        {
            const Core &core = m_cores[(offset - kMasterBase) / kMasterStride];
            switch ((offset - kMasterBase) % kMasterStride)
            {
            case 0x10u:
                return static_cast<uint16_t>(core.mvolL.current);
            case 0x12u:
                return static_cast<uint16_t>(core.mvolR.current);
            default:
                break;
            }
        }
        else if (offset < 2u * kCoreStride)
        {
            const int coreIndex = static_cast<int>(offset / kCoreStride);
            const uint32_t local = offset % kCoreStride;
            const Core &core = m_cores[coreIndex];
            if (local < kVoiceParamEnd)
                return readVoiceParam(core.voices[local >> 4u], (local & 0xFu) >> 1u);
            if (local >= kVoiceAddressBase && local < kVoiceAddressEnd)
            {
                const uint32_t rel = local - kVoiceAddressBase;
                return readVoiceAddress(core.voices[rel / 0xCu], (rel % 0xCu) >> 1u);
            }
            uint16_t value = 0u;
            if (readCoreRegister(coreIndex, local, value))
                return value;
        }
        return m_regs[offset / 2u];
    }

    void IopSpu2::write16(uint32_t offset, uint16_t value)
    {
        dodIopIoNote(0x1F900000u + (offset & (RegisterBytes - 2u)), true, value); // [DoD] DOD_IO_CENSUS
        offset &= (RegisterBytes - 1u) & ~1u;
        m_regs[offset / 2u] = value;
        // [DoD] Master area first: it lies inside core 1's 0x400..0x7FF block, and checked after the core block it was
        // never reached, so the game's MVOL, EVOL, AVOL and BVOL writes had no effect (reset values stayed).
        if (offset >= kMasterBase && offset < kMasterBase + 2u * kMasterStride)
        {
            Core &core = m_cores[(offset - kMasterBase) / kMasterStride];
            switch ((offset - kMasterBase) % kMasterStride)
            {
            case 0x0u:
                trace("core %u MVOLL=%04X at sample %u", (offset - kMasterBase) / kMasterStride, value, m_sampleCounter, 0u);
                core.mvolL.write(value);
                break;
            case 0x2u:
                core.mvolR.write(value);
                break;
            case 0x4u:
                trace("core %u EVOLL=%04X at sample %u", (offset - kMasterBase) / kMasterStride, value, m_sampleCounter, 0u);
                core.evolL = static_cast<int16_t>(value);
                break;
            case 0x6u:
                core.evolR = static_cast<int16_t>(value);
                break;
            case 0x8u:
                trace("core %u AVOLL=%04X at sample %u", (offset - kMasterBase) / kMasterStride, value, m_sampleCounter, 0u);
                core.avolL = static_cast<int16_t>(value);
                break;
            case 0xAu:
                core.avolR = static_cast<int16_t>(value);
                break;
            case 0xCu:
                trace("core %u BVOLL=%04X at sample %u", (offset - kMasterBase) / kMasterStride, value, m_sampleCounter, 0u);
                core.bvolL = static_cast<int16_t>(value);
                break;
            case 0xEu:
                core.bvolR = static_cast<int16_t>(value);
                break;
            default:
            {
                // [DoD] Reverb volumes IIR_VOL .. IN_COEF_R (+0x14..+0x26).
                const uint32_t rel = (offset - kMasterBase) % kMasterStride;
                if (rel >= kRevbVolBase && rel < kRevbVolBase + 2u * 10u)
                    core.revbVol[(rel - kRevbVolBase) / 2u] = static_cast<int16_t>(value);
                break;
            }
            }
            return;
        }
        if (offset < 2u * kCoreStride)
        {
            const int coreIndex = static_cast<int>(offset / kCoreStride);
            const uint32_t local = offset % kCoreStride;
            Core &core = m_cores[coreIndex];
            if (local < kVoiceParamEnd)
            {
                writeVoiceParam(core, core.voices[local >> 4u], (local & 0xFu) >> 1u, value);
                return;
            }
            if (local >= kVoiceAddressBase && local < kVoiceAddressEnd)
            {
                const uint32_t rel = local - kVoiceAddressBase;
                writeVoiceAddress(core.voices[rel / 0xCu], (rel % 0xCu) >> 1u, value);
                return;
            }
            writeCoreRegister(coreIndex, local, value);
            return;
        }
        if (offset == kSpdifIrqInfo)
            m_regs[offset / 2u] = 0u;
    }

    uint16_t IopSpu2::readVoiceParam(const Voice &voice, uint32_t reg) const
    {
        switch (reg)
        {
        case 0:
            return voice.volL.reg;
        case 1:
            return voice.volR.reg;
        case 2:
            return voice.pitch;
        case 3:
            return voice.adsr1;
        case 4:
            return voice.adsr2;
        case 5:
            return static_cast<uint16_t>(voice.envLevel);
        case 6:
            return static_cast<uint16_t>(voice.volL.current);
        case 7:
            return static_cast<uint16_t>(voice.volR.current);
        default:
            return 0u;
        }
    }

    void IopSpu2::writeVoiceParam(Core &, Voice &voice, uint32_t reg, uint16_t value)
    {
        switch (reg)
        {
        case 0:
            voice.volL.write(value);
            break;
        case 1:
            voice.volR.write(value);
            break;
        case 2:
            voice.pitch = value;
            break;
        case 3:
            voice.adsr1 = value;
            break;
        case 4:
            voice.adsr2 = value;
            break;
        case 5:
            voice.envLevel = std::min<int32_t>(value, 0x7FFF);
            break;
        default:
            break;
        }
    }

    uint16_t IopSpu2::readVoiceAddress(const Voice &voice, uint32_t reg) const
    {
        uint32_t address = 0u;
        switch (reg >> 1u)
        {
        case 0:
            address = voice.ssa;
            break;
        case 1:
            address = voice.lsax;
            break;
        default:
            // NAX points at the halfword holding the sample being played, past the block header.
            address = voice.nax;
            if (voice.phase != Phase::Off)
                address = (address + 1u + std::min<uint32_t>(voice.counter >> 12u, 27u) / 4u) & kAddressMask;
            break;
        }
        return (reg & 1u) == 0u ? static_cast<uint16_t>(address >> 16u) : static_cast<uint16_t>(address);
    }

    void IopSpu2::writeVoiceAddress(Voice &voice, uint32_t reg, uint16_t value)
    {
        uint32_t *target = nullptr;
        switch (reg >> 1u)
        {
        case 0:
            target = &voice.ssa;
            break;
        case 1:
            target = &voice.lsax;
            break;
        default:
            target = &voice.nax;
            voice.blockLoaded = false;
            break;
        }
        *target = (reg & 1u) == 0u ? setAddressHigh(*target, value) : setAddressLow(*target, value);
    }

    bool IopSpu2::readCoreRegister(int coreIndex, uint32_t local, uint16_t &value) const
    {
        const Core &core = m_cores[coreIndex];
        switch (local)
        {
        case 0x19Cu:
            value = static_cast<uint16_t>(core.irqa >> 16u);
            return true;
        case 0x19Eu:
            value = static_cast<uint16_t>(core.irqa);
            return true;
        case 0x1A8u:
            value = static_cast<uint16_t>(core.tsa >> 16u);
            return true;
        case 0x1AAu:
            value = static_cast<uint16_t>(core.tsa);
            return true;
        case 0x340u:
            value = static_cast<uint16_t>(core.endx);
            return true;
        case 0x342u:
            value = static_cast<uint16_t>(core.endx >> 16u);
            return true;
        case 0x344u:
            value = core.statx;
            return true;
        default:
            return false;
        }
    }

    void IopSpu2::writeCoreRegister(int coreIndex, uint32_t local, uint16_t value)
    {
        Core &core = m_cores[coreIndex];
        switch (local)
        {
        case 0x180u:
            core.pmon = setLow16(core.pmon, value);
            break;
        case 0x182u:
            core.pmon = setHigh8(core.pmon, value);
            break;
        case 0x184u:
            core.non = setLow16(core.non, value);
            break;
        case 0x186u:
            core.non = setHigh8(core.non, value);
            break;
        case 0x188u:
            core.vmixl = setLow16(core.vmixl, value);
            break;
        case 0x18Au:
            core.vmixl = setHigh8(core.vmixl, value);
            break;
        case 0x18Cu:
            core.vmixel = setLow16(core.vmixel, value);
            break;
        case 0x18Eu:
            core.vmixel = setHigh8(core.vmixel, value);
            break;
        case 0x190u:
            core.vmixr = setLow16(core.vmixr, value);
            break;
        case 0x192u:
            core.vmixr = setHigh8(core.vmixr, value);
            break;
        case 0x194u:
            core.vmixer = setLow16(core.vmixer, value);
            break;
        case 0x196u:
            core.vmixer = setHigh8(core.vmixer, value);
            break;
        case 0x198u:
            trace("core %u MMIX=%04X", static_cast<uint32_t>(coreIndex), value, 0u, 0u);
            core.mmix = value;
            break;
        case 0x19Au:
            if (((core.attr ^ value) & 0x80u) != 0u)
                trace("core %u ATTR=%04X (effects %u) at sample %u", static_cast<uint32_t>(coreIndex), value,
                      (value & 0x80u) != 0u ? 1u : 0u, m_sampleCounter);
            core.attr = value;
            break;
        case 0x19Cu:
            core.irqa = setAddressHigh(core.irqa, value);
            break;
        case 0x19Eu:
            core.irqa = setAddressLow(core.irqa, value);
            break;
        case 0x1A0u:
            keyOn(core, value);
            break;
        case 0x1A2u:
            keyOn(core, static_cast<uint32_t>(value & 0xFFu) << 16u);
            break;
        case 0x1A4u:
            keyOff(core, value);
            break;
        case 0x1A6u:
            keyOff(core, static_cast<uint32_t>(value & 0xFFu) << 16u);
            break;
        case 0x1A8u:
            core.tsa = setAddressHigh(core.tsa, value);
            break;
        case 0x1AAu:
            core.tsa = setAddressLow(core.tsa, value);
            break;
        case 0x1ACu:
            checkIrq(core.tsa, 1u);
            m_ram[core.tsa] = value;
            core.tsa = (core.tsa + 1u) & kAddressMask;
            break;
        case 0x1B0u:
            trace("core %u ADMAS=%04X (queued %u blocks) at sample %u", static_cast<uint32_t>(coreIndex), value,
                  static_cast<uint32_t>(core.autoDma.size()), m_sampleCounter);
            core.admas = value;
            if (!autoDmaEnabled(coreIndex))
                autoDmaStop(coreIndex);
            break;
        case 0x340u:
            core.endx &= 0xFF0000u;
            break;
        case 0x342u:
            core.endx &= 0xFFFFu;
            break;
        default:
            // [DoD] Reverb: ESA, the 22 address offsets and EEA hold the written halfwords as they are (PCSX2 keeps
            // them as 32-bit values; RevbGetIndexer masks).
            if (local == kRevbEsa)
                core.esa = (core.esa & 0xFFFFu) | (static_cast<uint32_t>(value) << 16u);
            else if (local == kRevbEsa + 2u)
                core.esa = (core.esa & 0xFFFF0000u) | value;
            else if (local >= kRevbAddrBase && local < kRevbEea)
            {
                uint32_t &target = core.revbAddr[(local - kRevbAddrBase) / 4u];
                target = ((local - kRevbAddrBase) & 2u) == 0u ? (target & 0xFFFFu) | (static_cast<uint32_t>(value) << 16u)
                                                                : (target & 0xFFFF0000u) | value;
            }
            else if (local == kRevbEea)
                core.eea = (core.eea & 0xFFFFu) | (static_cast<uint32_t>(value) << 16u);
            break;
        }
    }

    void IopSpu2::keyOn(Core &core, uint32_t mask)
    {
        for (int v = 0; v < 24; ++v)
        {
            if ((mask & (1u << v)) == 0u)
                continue;
            Voice &voice = core.voices[v];
            voice.nax = voice.ssa;
            voice.counter = 0u;
            voice.blockLoaded = false;
            voice.adpcm1 = 0;
            voice.adpcm2 = 0;
            voice.samples.fill(0);
            voice.envLevel = 0;
            voice.phase = Phase::Attack;
            voice.env.reset(static_cast<uint8_t>((voice.adsr1 >> 8u) & 0x7Fu), false, (voice.adsr1 & 0x8000u) != 0u);
            core.endx &= ~(1u << v);
        }
    }

    void IopSpu2::keyOff(Core &core, uint32_t mask)
    {
        for (int v = 0; v < 24; ++v)
        {
            if ((mask & (1u << v)) == 0u)
                continue;
            Voice &voice = core.voices[v];
            if (voice.phase == Phase::Off || voice.phase == Phase::Release)
                continue;
            voice.phase = Phase::Release;
            voice.env.reset(static_cast<uint8_t>((voice.adsr2 & 0x1Fu) << 2u), true, (voice.adsr2 & 0x20u) != 0u);
        }
    }

    void IopSpu2::loadBlock(Core &, Voice &voice)
    {
        const uint32_t base = voice.nax & ~7u;
        checkIrq(base, 8u);
        const uint16_t header = m_ram[base];
        voice.blockFlags = static_cast<uint8_t>(header >> 8u);
        if ((voice.blockFlags & 4u) != 0u)
            voice.lsax = base;

        uint32_t shift = header & 0xFu;
        if (shift > 12u)
            shift = 9u;
        const uint32_t filter = std::min<uint32_t>((header >> 4u) & 7u, 4u);
        for (uint32_t i = 0; i < 28u; ++i)
        {
            const uint16_t word = m_ram[(base + 1u + i / 4u) & kAddressMask];
            const uint16_t nibble = static_cast<uint16_t>((word >> ((i & 3u) * 4u)) & 0xFu);
            int32_t sample = static_cast<int16_t>(static_cast<uint16_t>(nibble << 12u)) >> shift;
            sample += (voice.adpcm1 * kAdpcmPos[filter] + voice.adpcm2 * kAdpcmNeg[filter] + 32) >> 6;
            sample = clamp16(sample);
            voice.adpcm2 = voice.adpcm1;
            voice.adpcm1 = sample;
            voice.samples[3u + i] = static_cast<int16_t>(sample);
        }
        voice.blockLoaded = true;
    }

    void IopSpu2::tickEnvelope(Voice &voice)
    {
        switch (voice.phase)
        {
        case Phase::Off:
            return;
        // [DoD] As PCSX2's V_ADSR: one counter for attack, decay and sustain (a phase change keeps it), and a
        // sustain that reaches 0 stops the voice.
        case Phase::Attack:
            voice.envLevel = voice.env.tick(voice.envLevel);
            if (voice.envLevel >= 0x7FFF)
            {
                const uint32_t counter = voice.env.counter;
                voice.phase = Phase::Decay;
                voice.env.reset(static_cast<uint8_t>(((voice.adsr1 >> 4u) & 0xFu) << 2u), true, true);
                voice.env.counter = counter;
            }
            return;
        case Phase::Decay:
        {
            voice.envLevel = voice.env.tick(voice.envLevel);
            const int32_t sustainLevel = std::min<int32_t>(((voice.adsr1 & 0xFu) + 1u) * 0x800u, 0x7FFF);
            if (voice.envLevel <= sustainLevel)
            {
                const uint32_t counter = voice.env.counter;
                voice.phase = Phase::Sustain;
                voice.env.reset(static_cast<uint8_t>((voice.adsr2 >> 6u) & 0x7Fu),
                                (voice.adsr2 & 0x4000u) != 0u,
                                (voice.adsr2 & 0x8000u) != 0u);
                voice.env.counter = counter;
            }
            return;
        }
        case Phase::Sustain:
            voice.envLevel = voice.env.tick(voice.envLevel);
            if (voice.envLevel == 0)
                voice.phase = Phase::Off;
            return;
        case Phase::Release:
            voice.envLevel = voice.env.tick(voice.envLevel);
            if (voice.envLevel <= 0)
            {
                voice.envLevel = 0;
                voice.phase = Phase::Off;
            }
            return;
        }
    }

    int32_t IopSpu2::sampleVoice(Core &core, int voiceIndex, int32_t previousOutput)
    {
        Voice &voice = core.voices[voiceIndex];
        // [DoD] PCSX2 MixVoice order: the volume slides every sample, playing or not; the envelope is computed before
        // it is applied to this sample (it used to be applied first and stepped after).
        voice.volL.tick();
        voice.volR.tick();
        if (voice.phase == Phase::Off)
        {
            voice.lastOutput = 0;
            return 0;
        }
        if (!voice.blockLoaded)
            loadBlock(core, voice);

        // [DoD] 4-point Gaussian interpolation of the SPU2 (PCSX2 GetVoiceValues; was Hermite): samples[index..
        // index+3] weighted by the table row of the counter's fraction bits 4-11, each product shifted on its own.
        const uint32_t index = voice.counter >> 12u;
        const auto &gauss = kSpu2GaussTable[(voice.counter & 0xFF0u) >> 4u];
        int32_t sample = 0;
        for (uint32_t k = 0; k < 4u; ++k)
            sample += (gauss[k] * voice.samples[index + k]) >> 15;

        tickEnvelope(voice);
        const int32_t output = (sample * voice.envLevel) >> 15;
        voice.lastOutput = output;

        uint32_t step = voice.pitch;
        if (voiceIndex > 0 && (core.pmon & (1u << voiceIndex)) != 0u)
            step = static_cast<uint32_t>(std::max<int32_t>(0, (static_cast<int32_t>(step) * (0x8000 + previousOutput)) >> 15));
        step = std::min<uint32_t>(step, 0x3FFFu);

        voice.counter += step;
        if ((voice.counter >> 12u) >= 28u)
        {
            voice.counter -= 28u << 12u;
            voice.samples[0] = voice.samples[28];
            voice.samples[1] = voice.samples[29];
            voice.samples[2] = voice.samples[30];
            voice.blockLoaded = false;
            if ((voice.blockFlags & 1u) != 0u)
            {
                core.endx |= 1u << voiceIndex;
                voice.nax = voice.lsax;
                if ((voice.blockFlags & 2u) == 0u)
                {
                    voice.phase = Phase::Off;
                    voice.envLevel = 0;
                }
            }
            else
            {
                voice.nax = ((voice.nax & ~7u) + 8u) & kAddressMask;
            }
        }
        return output;
    }

    // [DoD] PCSX2 V_Core::RevbGetIndexer: ESA + offset, moving one halfword every two samples, wrapped in the effects
    // area (EEA rounded up to the end of its 64 K halfword page). The sample counter plays PCSX2's Cycles.
    uint32_t IopSpu2::reverbIndex(const Core &core, uint32_t offset) const
    {
        const uint32_t start = core.esa & 0x3FFFFFu;
        const uint32_t end = (core.eea & 0x3FFFFFu) | 0xFFFFu;
        const uint32_t size = (end - start) + 1u;
        uint32_t x = (m_sampleCounter >> 1u) + offset;
        if (size != 0u) // only 0 for a start past the end that ESA < EEA let through; PCSX2 would divide by zero
            x %= size;
        x += start;
        return x & 0xFFFFFu;
    }

    // [DoD] PCSX2 V_Core::DoReverb (Reverb.cpp): one channel per sample (left on even sample counts), 39-tap down /
    // up-sampling around it. The work area in sound RAM is read always and written only with FxEnable (ATTR bit 7).
    void IopSpu2::doReverb(Core &core, int32_t inL, int32_t inR, int32_t &outL, int32_t &outR)
    {
        outL = 0;
        outR = 0;
        if (core.esa >= core.eea)
            return;

        const uint32_t pos = core.revbPos;
        core.revbDown[0][pos] = core.revbDown[0][pos | 64u] = static_cast<int16_t>(clamp16(inL));
        core.revbDown[1][pos] = core.revbDown[1][pos | 64u] = static_cast<int16_t>(clamp16(inR));

        const bool right = (m_sampleCounter & 1u) != 0u;
        const std::array<uint32_t, 22> &a = core.revbAddr;
        const uint32_t sameSrc = reverbIndex(core, right ? a[SAME_R_SRC] : a[SAME_L_SRC]);
        const uint32_t sameDst = reverbIndex(core, right ? a[SAME_R_DST] : a[SAME_L_DST]);
        const uint32_t samePrv = reverbIndex(core, (right ? a[SAME_R_DST] : a[SAME_L_DST]) - 1u);
        const uint32_t diffSrc = reverbIndex(core, right ? a[DIFF_L_SRC] : a[DIFF_R_SRC]);
        const uint32_t diffDst = reverbIndex(core, right ? a[DIFF_R_DST] : a[DIFF_L_DST]);
        const uint32_t diffPrv = reverbIndex(core, (right ? a[DIFF_R_DST] : a[DIFF_L_DST]) - 1u);
        const uint32_t comb1 = reverbIndex(core, right ? a[COMB1_R_SRC] : a[COMB1_L_SRC]);
        const uint32_t comb2 = reverbIndex(core, right ? a[COMB2_R_SRC] : a[COMB2_L_SRC]);
        const uint32_t comb3 = reverbIndex(core, right ? a[COMB3_R_SRC] : a[COMB3_L_SRC]);
        const uint32_t comb4 = reverbIndex(core, right ? a[COMB4_R_SRC] : a[COMB4_L_SRC]);
        const uint32_t apf1Src = reverbIndex(core, (right ? a[APF1_R_DST] : a[APF1_L_DST]) - a[APF1_SIZE]);
        const uint32_t apf1Dst = reverbIndex(core, right ? a[APF1_R_DST] : a[APF1_L_DST]);
        const uint32_t apf2Src = reverbIndex(core, (right ? a[APF2_R_DST] : a[APF2_L_DST]) - a[APF2_SIZE]);
        const uint32_t apf2Dst = reverbIndex(core, right ? a[APF2_R_DST] : a[APF2_L_DST]);

        const bool fxEnable = (core.attr & 0x80u) != 0u;
        if (fxEnable)
            for (const uint32_t address : {sameSrc, diffSrc, sameDst, diffDst, samePrv, diffPrv, comb1, comb2, comb3, comb4,
                                           apf1Dst, apf1Src, apf2Dst, apf2Src})
                checkIrq(address, 1u);

        const auto mem = [this](uint32_t address) { return static_cast<int32_t>(static_cast<int16_t>(m_ram[address])); };
        const std::array<int16_t, 10> &v = core.revbVol;
        const int32_t in = revbMul(right ? v[IN_COEF_R] : v[IN_COEF_L], reverbFir(core.revbDown[right ? 1 : 0], pos, kRevbDownCoefs));
        const int32_t same = revbMul(v[IIR_VOL], in + revbMul(v[WALL_VOL], mem(sameSrc)) - mem(samePrv)) + mem(samePrv);
        const int32_t diff = revbMul(v[IIR_VOL], in + revbMul(v[WALL_VOL], mem(diffSrc)) - mem(diffPrv)) + mem(diffPrv);
        int32_t out = revbMul(v[COMB1_VOL], mem(comb1)) + revbMul(v[COMB2_VOL], mem(comb2)) + revbMul(v[COMB3_VOL], mem(comb3)) +
                      revbMul(v[COMB4_VOL], mem(comb4));
        const int32_t apf1 = out - revbMul(v[APF1_VOL], mem(apf1Src));
        out = mem(apf1Src) + revbMul(v[APF1_VOL], apf1);
        const int32_t apf2 = out - revbMul(v[APF2_VOL], mem(apf2Src));
        out = mem(apf2Src) + revbMul(v[APF2_VOL], apf2);
        if (fxEnable)
        {
            m_ram[sameDst] = static_cast<uint16_t>(clamp16(same));
            m_ram[diffDst] = static_cast<uint16_t>(clamp16(diff));
            m_ram[apf1Dst] = static_cast<uint16_t>(clamp16(apf1));
            m_ram[apf2Dst] = static_cast<uint16_t>(clamp16(apf2));
        }
        out = clamp16(out);

        const int16_t value = static_cast<int16_t>(out);
        core.revbUp[right ? 1 : 0][pos] = core.revbUp[right ? 1 : 0][pos | 64u] = value;
        core.revbUp[right ? 0 : 1][pos] = core.revbUp[right ? 0 : 1][pos | 64u] = 0;
        core.revbPos = (pos + 1u) & 63u;
        outL = reverbFir(core.revbUp[0], core.revbPos, kRevbUpCoefs);
        outR = reverbFir(core.revbUp[1], core.revbPos, kRevbUpCoefs);
    }

    uint32_t IopSpu2::generate(int16_t *out, uint32_t frames)
    {
        struct NowAfter
        {
            const uint32_t &counter;
            ~NowAfter() { g_dodAdmaNow = counter + 1u; }
        } nowAfter{m_sampleCounter};
        for (uint32_t frame = 0; frame < frames; ++frame)
        {
            g_dodAdmaNow = m_sampleCounter + 1u;
            bool autoDmaEnded = false;
            int32_t coreOutL[2] = {0, 0};
            int32_t coreOutR[2] = {0, 0};
            for (int c = 0; c < 2; ++c)
            {
                Core &core = m_cores[c];
                int32_t dryL = 0;
                int32_t dryR = 0;
                int32_t wetL = 0; // [DoD] voices sent to the reverb (VMIXEL / VMIXER)
                int32_t wetR = 0;
                int32_t previous = 0;
                for (int v = 0; v < 24; ++v)
                {
                    const int32_t output = sampleVoice(core, v, previous);
                    previous = output;
                    if (output == 0)
                        continue;
                    const Voice &voice = core.voices[v];
                    const uint32_t bit = 1u << v;
                    const int32_t outL = (output * voice.volL.current) >> 15;
                    const int32_t outR = (output * voice.volR.current) >> 15;
                    if ((core.vmixl & bit) != 0u)
                        dryL += outL;
                    if ((core.vmixr & bit) != 0u)
                        dryR += outR;
                    if ((core.vmixel & bit) != 0u)
                        wetL += outL;
                    if ((core.vmixer & bit) != 0u)
                        wetR += outR;
                }

                // [DoD] Sound data input, as PCSX2's V_Core::ReadInput: the core's input area is always read and mixed
                // (MMIX 0x80/0x40), AutoDMA or not; AutoDMA only fills it, one half ahead of the read position (at 0x100
                // the first half, at 0 the second). So when a stream stops, what was already transferred plays out:
                // dropping the input at once when ADMAS was cleared cut DoD's title music to zero on START (a click).
                const uint32_t base = 0x2000u + static_cast<uint32_t>(c) * 0x400u + core.inputPos;
                checkIrq(base, 1u);
                checkIrq(base + 0x200u, 1u);
                const int32_t inputL = (static_cast<int16_t>(m_ram[base]) * core.bvolL) >> 15;
                const int32_t inputR = (static_cast<int16_t>(m_ram[base + 0x200u]) * core.bvolR) >> 15;
                if (autoDmaEnabled(c) && (core.inputPos == 0x100u || core.inputPos == 0u) &&
                    pullAutoDma(c, core.inputPos == 0x100u ? 0u : 0x100u))
                    autoDmaEnded = true;
                core.inputPos = (core.inputPos + 1u) & 0x1FFu;

                if (c == 1)
                {
                    m_stemInputL = inputL;
                    m_stemInputR = inputR;
                    m_stemVoicesL = dryL;
                    m_stemVoicesR = dryR;
                }
                // [DoD] Core 0's output comes in as core 1's external input, scaled by AVOL (BVOL is the sound data
                // input volume), as in PCSX2's SPU2; core 0 has none (its MMIX bits 0-3 are ignored).
                const int32_t extL = c == 1 ? (coreOutL[0] * core.avolL) >> 15 : 0;
                const int32_t extR = c == 1 ? (coreOutR[0] * core.avolR) >> 15 : 0;
                const uint32_t mmix = core.mmix & (c == 0 ? 0xFF0u : 0xFFFu);
                // The voice sums saturate before mixing (PCSX2 MixCore).
                dryL = clamp16(dryL);
                dryR = clamp16(dryR);
                wetL = clamp16(wetL);
                wetR = clamp16(wetR);

                // MMIX: dry gates (0x800/0x400 voices L/R, 0x80/0x40 sound data input = AutoDMA, 0x8/0x4 external =
                // core 0) and [DoD] wet gates into the reverb (0x200/0x100 voices, 0x20/0x10 input, 0x2/0x1 external).
                int32_t mixL = (mmix & 0x800u) != 0u ? dryL : 0;
                int32_t mixR = (mmix & 0x400u) != 0u ? dryR : 0;
                if ((mmix & 0x80u) != 0u)
                    mixL += inputL;
                if ((mmix & 0x40u) != 0u)
                    mixR += inputR;
                if ((mmix & 0x8u) != 0u)
                    mixL += extL;
                if ((mmix & 0x4u) != 0u)
                    mixR += extR;
                int32_t revInL = (mmix & 0x200u) != 0u ? wetL : 0;
                int32_t revInR = (mmix & 0x100u) != 0u ? wetR : 0;
                if ((mmix & 0x20u) != 0u)
                    revInL += inputL;
                if ((mmix & 0x10u) != 0u)
                    revInR += inputR;
                if ((mmix & 0x2u) != 0u)
                    revInL += extL;
                if ((mmix & 0x1u) != 0u)
                    revInR += extR;

                // [DoD] Reverb return through EVOL (PCSX2: TD + ApplyVolume(DoReverb(TW), FxVol)); master volume after.
                int32_t revL = 0;
                int32_t revR = 0;
                if (reverbOn())
                    doReverb(core, revInL, revInR, revL, revR);
                revL = (revL * core.evolL) >> 15;
                revR = (revR * core.evolR) >> 15;
                mixL += revL;
                mixR += revR;
                if (c == 1 && reverbStemsFile())
                {
                    const int16_t rs[4] = {static_cast<int16_t>(clamp16(revInL)), static_cast<int16_t>(clamp16(revInR)),
                                           static_cast<int16_t>(clamp16(revL)), static_cast<int16_t>(clamp16(revR))};
                    std::fwrite(rs, sizeof(rs), 1, reverbStemsFile());
                }

                core.mvolL.tick();
                core.mvolR.tick();
                coreOutL[c] = clamp16((clamp16(mixL) * core.mvolL.current) >> 15);
                coreOutR[c] = clamp16((clamp16(mixR) * core.mvolR.current) >> 15);
            }
            out[frame * 2u] = static_cast<int16_t>(coreOutL[1]);
            out[frame * 2u + 1u] = static_cast<int16_t>(coreOutR[1]);
            ++m_sampleCounter;
            if (g_dodSpu2StemsSink || stemsFile())
            {
                // [DoD] diagnostics: core 0 output L/R, core 1 sound data input L/R, core 1 voices L/R.
                const int16_t stems[6] = {static_cast<int16_t>(coreOutL[0]), static_cast<int16_t>(coreOutR[0]),
                                          static_cast<int16_t>(clamp16(m_stemInputL)), static_cast<int16_t>(clamp16(m_stemInputR)),
                                          static_cast<int16_t>(clamp16(m_stemVoicesL)), static_cast<int16_t>(clamp16(m_stemVoicesR))};
                if (g_dodSpu2StemsSink)
                    g_dodSpu2StemsSink(stems);
                if (stemsFile())
                    std::fwrite(stems, sizeof(stems), 1, stemsFile());
            }
            if (autoDmaEnded)
                return frame + 1u;
        }
        return frames;
    }

    bool IopSpu2::autoDmaEnabled(int coreIndex) const noexcept
    {
        return (m_cores[coreIndex & 1].admas & static_cast<uint16_t>((coreIndex & 1) + 1)) != 0u;
    }

    void IopSpu2::autoDmaStart(int coreIndex, const uint8_t *source, uint32_t bytes, uint32_t iopAddress)
    {
        static uint32_t s_startTraces = 0u;
        if (++s_startTraces <= 16u)
            trace("core %u AutoDMA start 0x%05X +%u bytes, inputPos %03X", static_cast<uint32_t>(coreIndex & 1), iopAddress, bytes, m_cores[coreIndex & 1].inputPos);
        if (admaLastPull() && s_startTraces <= 60u)
            std::fprintf(stderr, "[adma-watch] start core %u 0x%05X +%u at sample %u inputPos %03X queued %u\n",
                         static_cast<uint32_t>(coreIndex & 1), iopAddress, bytes, m_sampleCounter,
                         m_cores[coreIndex & 1].inputPos, static_cast<uint32_t>(m_cores[coreIndex & 1].autoDma.size()));
        if (source && bytes != 0u)
            m_cores[coreIndex & 1].autoDma.push_back(Core::AutoDmaBlock{source, bytes, 0u, iopAddress, m_sampleCounter});
    }

    void IopSpu2::autoDmaStop(int coreIndex)
    {
        Core &core = m_cores[coreIndex & 1];
        core.autoDma.clear();
        core.autoDmaDone = false;
    }

    bool IopSpu2::takeAutoDmaDone(int coreIndex) noexcept
    {
        Core &core = m_cores[coreIndex & 1];
        const bool done = core.autoDmaDone;
        core.autoDmaDone = false;
        return done;
    }

    // Copies the next 1 KB of the AutoDMA stream (0x200 bytes left, then 0x200 bytes right) into the given half (0 or
    // 0x100) of the input area; zeros when the driver has not queued enough. True when a DMA block ended.
    bool IopSpu2::pullAutoDma(int coreIndex, uint32_t half)
    {
        Core &core = m_cores[coreIndex & 1];
        std::array<uint8_t, 0x400> bytes{};
        uint32_t filled = 0u;
        bool ended = false;
        // [DoD] DOD_ADMA_WATCH: per left/right half of this 1 KB, halfwords not rewritten since their last pull
        // (stale) and the age of the newest / oldest write.
        uint32_t *const lastPull = admaLastPull();
        const uint32_t now = m_sampleCounter + 1u;
        uint32_t stale[2] = {0u, 0u}, firstStale[2] = {~0u, ~0u}, lastStale[2] = {0u, 0u};
        uint32_t newest[2] = {0u, 0u}, oldest[2] = {~0u, ~0u}, newestBlockWrite[2] = {0u, 0u};
        uint32_t srcFirst = 0u, blockAddress = 0u, blockSample = 0u;
        std::array<uint32_t, 0x200> writerPc{}, writerSrc{}, writerAt{}; // per halfword of this 1 KB
        while (filled < bytes.size() && !core.autoDma.empty())
        {
            Core::AutoDmaBlock &block = core.autoDma.front();
            const uint32_t take = std::min<uint32_t>(static_cast<uint32_t>(bytes.size()) - filled, block.bytes - block.offset);
            if (lastPull && g_dodAdmaLastWrite)
            {
                if (filled == 0u)
                {
                    srcFirst = block.iopAddress + block.offset;
                    blockAddress = block.iopAddress;
                    blockSample = block.startSample;
                }
                for (uint32_t i = 0; i < take; i += 2u)
                {
                    const uint32_t hw = ((block.iopAddress + block.offset + i) & 0x1FFFFFu) >> 1u;
                    const uint32_t pos = (filled + i) >> 1u;
                    const int side = pos < 0x100u ? 0 : 1;
                    const uint32_t local = pos & 0xFFu;
                    const uint32_t entry = g_dodAdmaLastWrite[hw];
                    const uint32_t written = entry & 0x7FFFFFFFu;
                    writerPc[pos] = g_dodAdmaLastPc[hw];
                    writerSrc[pos] = g_dodAdmaLastSrc[hw];
                    writerAt[pos] = written;
                    if (lastPull[hw] != 0u && written <= lastPull[hw])
                    {
                        ++stale[side];
                        firstStale[side] = std::min(firstStale[side], local);
                        lastStale[side] = std::max(lastStale[side], local);
                    }
                    if (written > newest[side])
                    {
                        newest[side] = written;
                        newestBlockWrite[side] = entry >> 31u;
                    }
                    oldest[side] = std::min(oldest[side], written);
                    lastPull[hw] = now;
                }
            }
            std::memcpy(bytes.data() + filled, block.data + block.offset, take);
            filled += take;
            block.offset += take;
            if (block.offset >= block.bytes)
            {
                core.autoDma.pop_front();
                ended = true;
            }
        }
        const uint32_t base = 0x2000u + static_cast<uint32_t>(coreIndex & 1) * 0x400u + (half & 0x100u);
        for (uint32_t i = 0; i < 0x100u; ++i)
        {
            uint16_t left;
            uint16_t right;
            std::memcpy(&left, bytes.data() + i * 2u, sizeof(left));
            std::memcpy(&right, bytes.data() + 0x200u + i * 2u, sizeof(right));
            m_ram[base + i] = left;
            m_ram[base + 0x200u + i] = right;
        }
        if (ended)
            core.autoDmaDone = true;
        if (filled < bytes.size() && (core.admas & static_cast<uint16_t>((coreIndex & 1) + 1)) != 0u)
            traceUnderrun(static_cast<uint32_t>(coreIndex & 1), filled, core.inputPos, m_sampleCounter);
        if (lastPull && g_dodAdmaLastWrite && filled != 0u)
        {
            static AdmaWatchStats stats;
            ++stats.pulls;
            if (stale[0] != 0u)
            {
                ++stats.staleL;
                stats.staleHwL += stale[0];
            }
            if (stale[1] != 0u)
            {
                ++stats.staleR;
                stats.staleHwR += stale[1];
            }
            const auto ago = [now](uint32_t written) { return written == 0u ? -1 : static_cast<int>(now - written); };
            if ((stats.pulls <= 40u || stale[0] != 0u || stale[1] != 0u) && stats.lines < 4000u)
            {
                ++stats.lines;
                std::fprintf(stderr,
                             "[adma-watch] pull sample %u core %u into %03X src 0x%05X (block 0x%05X started at %u, +%u) "
                             "L stale %u [%u..%u] R stale %u [%u..%u] | newest write ago L %d%s R %d%s, oldest ago L %d R %d%s\n",
                             m_sampleCounter, static_cast<uint32_t>(coreIndex & 1), half, srcFirst, blockAddress, blockSample,
                             m_sampleCounter - blockSample, stale[0], stale[0] ? firstStale[0] : 0u, stale[0] ? lastStale[0] : 0u,
                             stale[1], stale[1] ? firstStale[1] : 0u, stale[1] ? lastStale[1] : 0u, ago(newest[0]),
                             newestBlockWrite[0] ? " (block write)" : "", ago(newest[1]), newestBlockWrite[1] ? " (block write)" : "",
                             ago(oldest[0]), ago(oldest[1]), ended ? " ENDED" : "");
            }
            // Halfwords equal (and nonzero) to the same position 32 pulls (8192 samples) earlier: the right channel
            // replays old data there. Print who wrote this 1 KB, as runs of halfwords with one writer and a
            // contiguous source.
            static std::array<std::array<uint8_t, 0x400>, 64> history{};
            static uint32_t historyCount = 0u, repeatLines = 0u;
            if (filled == bytes.size())
            {
                uint32_t rep[2] = {0u, 0u}, firstRep[2] = {~0u, ~0u}, lastRep[2] = {0u, 0u};
                if (historyCount >= 32u)
                {
                    const auto &old = history[(historyCount - 32u) % 64u];
                    for (uint32_t pos = 0; pos < 0x200u; ++pos)
                    {
                        uint16_t a, b;
                        std::memcpy(&a, bytes.data() + pos * 2u, 2u);
                        std::memcpy(&b, old.data() + pos * 2u, 2u);
                        if (a == b && a != 0u)
                        {
                            const int side = pos < 0x100u ? 0 : 1;
                            ++rep[side];
                            firstRep[side] = std::min(firstRep[side], pos & 0xFFu);
                            lastRep[side] = std::max(lastRep[side], pos & 0xFFu);
                        }
                    }
                }
                const bool repeat = rep[0] > 16u || rep[1] > 16u;
                if ((repeat && repeatLines < 400u) || historyCount < 6u || (historyCount % 2000u) == 0u)
                {
                    ++repeatLines;
                    std::fprintf(stderr, "[adma-watch] %s sample %u src 0x%05X: equal to 8192 samples earlier L %u [%u..%u] R %u [%u..%u]; writers:",
                                 repeat ? "REPEAT" : "layout", m_sampleCounter, srcFirst, rep[0], rep[0] ? firstRep[0] : 0u,
                                 rep[0] ? lastRep[0] : 0u, rep[1], rep[1] ? firstRep[1] : 0u, rep[1] ? lastRep[1] : 0u);
                    uint32_t runStart = 0u;
                    for (uint32_t pos = 1; pos <= 0x200u; ++pos)
                    {
                        const bool split = pos == 0x200u || pos == 0x100u || writerPc[pos] != writerPc[pos - 1u] ||
                                           writerSrc[pos] != writerSrc[pos - 1u] + (writerSrc[pos - 1u] ? 2u : 0u);
                        if (!split)
                            continue;
                        std::fprintf(stderr, " %s%03X-%03X pc %05X src %05X age %d;", runStart < 0x100u ? "L" : "R",
                                     runStart & 0xFFu, (pos - 1u) & 0xFFu, writerPc[runStart], writerSrc[runStart],
                                     writerAt[runStart] ? static_cast<int>(now - writerAt[runStart]) : -1);
                        runStart = pos;
                    }
                    std::fprintf(stderr, "\n");
                }
                // A right-only replay (the fault, not repeated music): when each 32-sample group of this 1 KB was
                // written, and the IOP preemptions around that time.
                static uint32_t dumps = 0u;
                if (rep[1] >= 64u && rep[0] < 16u && dumps < 25u)
                {
                    if (dumps == 0u && g_dodDescribeIopThreads)
                        g_dodDescribeIopThreads();
                    ++dumps;
                    std::fprintf(stderr, "[adma-sched] right replay at sample %u src 0x%05X; write age per 32 samples L:", m_sampleCounter, srcFirst);
                    for (uint32_t g = 0; g < 8u; ++g)
                        std::fprintf(stderr, " %d", writerAt[g * 32u] ? static_cast<int>(now - writerAt[g * 32u]) : -1);
                    std::fprintf(stderr, " R:");
                    for (uint32_t g = 0; g < 8u; ++g)
                        std::fprintf(stderr, " %d", writerAt[0x100u + g * 32u] ? static_cast<int>(now - writerAt[0x100u + g * 32u]) : -1);
                    std::fprintf(stderr, "\n");
                    const uint32_t n = std::min<uint32_t>(g_dodSchedCount, 256u);
                    for (uint32_t k = n; k > 0u; --k)
                    {
                        const DodSchedEvent &e = g_dodSchedRing[(g_dodSchedCount - k) % 256u];
                        if (e.sample + 1400u < now)
                            continue;
                        std::fprintf(stderr, "[adma-sched]   preempted %d ago: thread %d (prio %d) at pc %05X -> thread %d (prio %d) at pc %05X\n",
                                     static_cast<int>(now - e.sample), e.fromId, e.fromPrio, e.fromPc, e.toId, e.toPrio, e.toPc);
                    }
                }
                history[historyCount % 64u] = bytes;
                ++historyCount;
            }
            if (m_sampleCounter >= stats.nextSummary)
            {
                std::fprintf(stderr, "[adma-watch] summary at %.1f s: %u pulls, stale left in %u (%u halfwords), stale right in %u (%u halfwords)\n",
                             m_sampleCounter / 48000.0, stats.pulls, stats.staleL, stats.staleHwL, stats.staleR, stats.staleHwR);
                stats.nextSummary = m_sampleCounter + 5u * 48000u;
            }
        }
        return ended;
    }

    void IopSpu2::dmaWrite(int coreIndex, const uint8_t *source, uint32_t bytes)
    {
        Core &core = m_cores[coreIndex & 1];
        const uint32_t halfwords = bytes / 2u;
        checkIrq(core.tsa, halfwords);
        for (uint32_t i = 0; i < halfwords; ++i)
        {
            uint16_t value;
            std::memcpy(&value, source + i * 2u, sizeof(value));
            m_ram[core.tsa] = value;
            core.tsa = (core.tsa + 1u) & kAddressMask;
        }
        core.statx |= 0x80u;
    }

    void IopSpu2::dmaRead(int coreIndex, uint8_t *destination, uint32_t bytes)
    {
        Core &core = m_cores[coreIndex & 1];
        const uint32_t halfwords = bytes / 2u;
        checkIrq(core.tsa, halfwords);
        for (uint32_t i = 0; i < halfwords; ++i)
        {
            std::memcpy(destination + i * 2u, &m_ram[core.tsa], sizeof(uint16_t));
            core.tsa = (core.tsa + 1u) & kAddressMask;
        }
        core.statx |= 0x80u;
    }
}

