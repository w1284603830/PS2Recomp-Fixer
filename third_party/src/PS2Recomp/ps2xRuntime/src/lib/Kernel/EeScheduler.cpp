#include "runtime/ee_scheduler.h"
#include "../ps2_iop_post.h"
#include "../gs/dod_pcsx2_gs.h"

extern bool g_ps2GuestUnwinding;
void dodCallHistTick(uint64_t field); // [DoD] ps2_runtime.cpp
extern int g_dodCurrentThreadId;       // [DoD] ps2_runtime.cpp diagnostics

// [DoD] Contexts made by StartThread started with VU0 vf0 = (0,0,0,0) instead of the hardwired (0,0,0,1): matrix code
// such as VMADDw ACC, vf4, vf0w and VDIV Q, vf0w, vfw lost every translation and divided by zero on those threads.
// vf0/vi0 are now set before every guest slice. VU0 registers stay per thread (each context has its own, as the
// Urban Reign port also runs it): sharing one VU0 between threads (DOD_VU0_SHARED=1, an experiment) broke the CRI
// movie decoder thread, which uses VU0 while the main thread does too.
namespace
{
    constexpr size_t kDodVu0Begin = offsetof(R5900Context, vu0_vf);
    constexpr size_t kDodVu0End = offsetof(R5900Context, cop0_index);
    alignas(16) uint8_t g_dodVu0Block[kDodVu0End - kDodVu0Begin];
    uint32_t g_dodVu0Ccr[32];
    bool g_dodVu0Valid = false;

    bool dodVu0Shared()
    {
        static const bool shared = std::getenv("DOD_VU0_SHARED") != nullptr;
        return shared;
    }

    // [DoD] The random register R is the one VU0 register every thread shares (2026-10-27). The game seeds R once,
    // through a script command on the main thread (0x14CDA8 -> 0x2B8590: R from its Mersenne Twister when R's mantissa
    // is 0), and its enemy thread (entry 0x155250) then draws with VRNEXT from the same R, as on the one VU0 of the PS2.
    // With R per thread that thread kept R = 1.0 (mantissa 0, which the LFSR never leaves) and drew the same number
    // thousands of times per fight: enemies repeated one choice (DOD_RNG_TRACE). DOD_VU0_SHARED_R=0: R per thread again.
    alignas(16) __m128 g_dodVu0R;
    bool g_dodVu0RValid = false;

    bool dodVu0SharedR()
    {
        static const bool shared = [] { const char *v = std::getenv("DOD_VU0_SHARED_R"); return !(v && *v == '0'); }();
        return shared;
    }

    void dodVu0Load(R5900Context &context)
    {
        if (dodVu0Shared() && g_dodVu0Valid)
        {
            std::memcpy(reinterpret_cast<uint8_t *>(&context) + kDodVu0Begin, g_dodVu0Block, sizeof(g_dodVu0Block));
            std::memcpy(context.cop2_ccr, g_dodVu0Ccr, sizeof(g_dodVu0Ccr));
        }
        else if (dodVu0SharedR() && g_dodVu0RValid)
        {
            context.vu0_r = g_dodVu0R;
        }
        context.vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
        context.vi[0] = 0;
    }

    void dodVu0Save(const R5900Context &context)
    {
        if (dodVu0SharedR())
        {
            g_dodVu0R = context.vu0_r;
            g_dodVu0RValid = true;
        }
        if (!dodVu0Shared())
            return;
        std::memcpy(g_dodVu0Block, reinterpret_cast<const uint8_t *>(&context) + kDodVu0Begin, sizeof(g_dodVu0Block));
        std::memcpy(g_dodVu0Ccr, context.cop2_ccr, sizeof(g_dodVu0Ccr));
        g_dodVu0Valid = true;
    }

    // [DoD] DI/EI clear and set Status.EIE. While the running thread has interrupts off, a pending interrupt (or other
    // async invocation) stays queued and the time slice does not preempt it; both happen at the first checkpoint after
    // EI (upstream PS2Recomp #302 and #222). Dawn of Dreams uses DI around its heap allocator (0x10E620), DIntr and
    // one kernel-mode store. EXL is not checked: only raiseCop0Exception sets it and nothing returns from there.
    bool dodGuestInterruptsEnabled(const R5900Context &context) noexcept
    {
        static const bool ignore = std::getenv("DOD_IGNORE_DI") != nullptr;
        return ignore || (context.cop0_status & 0x10001u) == 0x10001u;
    }
    bool g_dodInvocationsHeld = false; // a pending invocation waits for EI

    // [DoD] KillzoneRecomp patch 0018: events queued in m_events, readable without the lock. processPendingEvents runs
    // on nearly every scheduler iteration and used to lock twice, swap a std::deque and read the host clock (deadline
    // scan) even with nothing to do. DOD_EVENT_PUMP_ALWAYS=1: the old full pass every time.
    std::atomic<uint32_t> g_dodQueuedEvents{0};
    bool dodEventPumpAlways()
    {
        static const bool always = std::getenv("DOD_EVENT_PUMP_ALWAYS") != nullptr;
        return always;
    }
}                                    // [DoD] ps2_runtime.cpp

// [DoD] Interrupt causes raised while masked (DisableIntc/DisableDmac or an INTC handler running with its cause off):
// on the hardware the status bit stays set and the interrupt is taken when the cause is enabled again. They were
// dropped (Urban Reign port, PROGRESS "Bugs do runtime" 5).
namespace
{
    uint32_t g_dodPendingMaskedIntc = 0u;
    uint32_t g_dodPendingMaskedDmac = 0u;
}

// D_STAT write-one-to-clear (ps2_memory.cpp): a cleared channel status has no interrupt left to deliver when the cause
// is enabled again (Urban Reign port, PROGRESS 92: libmpeg's split CSC ran its second half too early otherwise).
void dodEeClearLatchedDmac(uint32_t channels)
{
    g_dodPendingMaskedDmac &= ~channels;
}
class PS2Memory;
void dodRaiseIntcStat(PS2Memory &memory, uint32_t bits);            // [DoD] ps2_memory.cpp

#include "ps2_log.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <atomic>
#include <unordered_map>

extern std::atomic<int64_t> g_dodVBlankPeriodNs; // [DoD] defined below processEvent
static inline std::chrono::nanoseconds dodVBlankHostPeriod()
{
    return std::chrono::nanoseconds(g_dodVBlankPeriodNs.load(std::memory_order_relaxed));
}

namespace
{
    // [DoD] DOD_DISPATCH_STATS=1 (RUNTIME_PATCHES row 119): once a second, how often guest code went back to the
    // dispatcher and why. Executor-thread counters except the posted events (any thread). No behaviour change.
    struct DodDispatchStats
    {
        uint64_t loops = 0, guestRuns = 0, hops = 0, preRunCheckpoint = 0, idleWaits = 0, switches = 0, completions = 0;
        uint64_t cpCalls = 0, cpFlag = 0, cpStop = 0, cpDeadline = 0, cpHeld = 0, cpSlice = 0;
        uint64_t setTimerIrq = 0, setInvocation = 0, setPreempt = 0, setPendingWork = 0, setWake = 0;
        uint64_t processed[8] = {};
        std::atomic<uint64_t> posted[8] = {};
        uint64_t threadRuns[64] = {};
        std::unordered_map<uint32_t, uint64_t> runPc;   // pc the dispatcher started guest code at
        std::unordered_map<uint64_t, uint64_t> exitPc;  // pc (and unwinding bit 32) guest code came back with
        std::chrono::steady_clock::time_point last{};
    };

    std::string dodTopCounts(const std::unordered_map<uint64_t, uint64_t> &counts)
    {
        std::vector<std::pair<uint64_t, uint64_t>> v(counts.begin(), counts.end());
        std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
        std::string out;
        char item[48];
        for (size_t i = 0; i < v.size() && i < 8u; ++i)
        {
            std::snprintf(item, sizeof(item), " %06X%s:%llu", static_cast<unsigned>(v[i].first & 0xFFFFFFFFu),
                          (v[i].first >> 32) ? "u" : "", static_cast<unsigned long long>(v[i].second));
            out += item;
        }
        return out;
    }
    DodDispatchStats g_dodDispatch;

    inline bool dodDispatchStatsOn()
    {
        static const bool on = std::getenv("DOD_DISPATCH_STATS") != nullptr;
        return on;
    }

    inline bool dodFastHopOn()
    {
        static const bool on = []() { const char *v = std::getenv("DOD_FAST_HOP"); return !(v && v[0] == '0'); }();
        return on;
    }

    void dodDispatchStatsPrint(uint64_t eeCycle)
    {
        DodDispatchStats &s = g_dodDispatch;
        const auto now = std::chrono::steady_clock::now();
        if (s.last == std::chrono::steady_clock::time_point{})
        {
            s.last = now;
            return;
        }
        if (now - s.last < std::chrono::seconds(1))
            return;
        static const char *const kTypes[] = {"stop", "vbs", "vbe", "dmac", "wake", "alarm", "?", "?"};
        std::string events, posted, threads;
        for (int i = 1; i < 6; ++i)
        {
            events += ' ' + std::string(kTypes[i]) + '=' + std::to_string(s.processed[i]);
            posted += ' ' + std::string(kTypes[i]) + '=' + std::to_string(s.posted[i].exchange(0));
        }
        for (int i = 0; i < 64; ++i)
            if (s.threadRuns[i] != 0u)
                threads += ' ' + std::to_string(i) + ':' + std::to_string(s.threadRuns[i]);
        std::fprintf(stderr,
                     "[dispatch] cycle %llu loops %llu runs %llu hops %llu pre-run-cp %llu idle %llu switches %llu completions %llu"
                     " | cp true: flag %llu stop %llu deadline %llu held %llu slice %llu"
                     " | flag set: timer %llu invocation %llu preempt %llu pending-work %llu wake %llu"
                     " | processed%s | posted%s | runs by thread%s\n",
                     static_cast<unsigned long long>(eeCycle), static_cast<unsigned long long>(s.loops),
                     static_cast<unsigned long long>(s.guestRuns), static_cast<unsigned long long>(s.hops),
                     static_cast<unsigned long long>(s.preRunCheckpoint),
                     static_cast<unsigned long long>(s.idleWaits), static_cast<unsigned long long>(s.switches),
                     static_cast<unsigned long long>(s.completions),
                     static_cast<unsigned long long>(s.cpFlag), static_cast<unsigned long long>(s.cpStop),
                     static_cast<unsigned long long>(s.cpDeadline), static_cast<unsigned long long>(s.cpHeld),
                     static_cast<unsigned long long>(s.cpSlice), static_cast<unsigned long long>(s.setTimerIrq),
                     static_cast<unsigned long long>(s.setInvocation), static_cast<unsigned long long>(s.setPreempt),
                     static_cast<unsigned long long>(s.setPendingWork), static_cast<unsigned long long>(s.setWake),
                     events.c_str(), posted.c_str(), threads.c_str());
        {
            std::unordered_map<uint64_t, uint64_t> runs(s.runPc.begin(), s.runPc.end());
            std::fprintf(stderr, "[dispatch]   run at%s | came back at (u = unwinding)%s\n", dodTopCounts(runs).c_str(),
                         dodTopCounts(s.exitPc).c_str());
            s.runPc.clear();
            s.exitPc.clear();
        }
        const auto keep = s.last;
        s.loops = s.guestRuns = s.hops = s.preRunCheckpoint = s.idleWaits = s.switches = s.completions = 0;
        s.cpCalls = s.cpFlag = s.cpStop = s.cpDeadline = s.cpHeld = s.cpSlice = 0;
        s.setTimerIrq = s.setInvocation = s.setPreempt = s.setPendingWork = s.setWake = 0;
        for (auto &v : s.processed)
            v = 0;
        for (auto &v : s.threadRuns)
            v = 0;
        s.last = keep + std::chrono::seconds(1) > now - std::chrono::seconds(1) ? keep + std::chrono::seconds(1) : now;
    }
}

namespace
{
    // [DoD] Bounded, executor-local diagnostics; no scheduler state or timing is
    // changed. Track only real semaphore/external blocks, not successful polls.
    struct DodWaitPerf
    {
        using Clock = std::chrono::steady_clock;
        struct Bucket
        {
            bool used = false;
            EeWaitReason reason = EeWaitReason::None;
            int thread = 0;
            uint32_t object = 0, pc = 0, ra = 0;
            uint64_t started = 0, released = 0, resumed = 0, errors = 0, abandoned = 0;
            double blockedMs = 0, blockedMaxMs = 0, readyMs = 0, readyMaxMs = 0;
        };
        struct Active
        {
            unsigned phase = 0, bucket = 0; // 1 blocked, 2 released/suspended, 3 ready
            size_t invocationDepth = 0;
            Clock::time_point blockedAt{}, readyAt{};
        };
        const EeScheduler *owner = nullptr;
        std::array<Active, 256> active{};
        std::array<Bucket, 32> buckets{};
        Clock::time_point windowStart{};
        uint64_t dropped = 0;
        unsigned lines = 0;

        Active *entry(int id) noexcept
        { return id > 0 && id < static_cast<int>(active.size()) ? &active[id] : nullptr; }
        void begin(const GuestThread &thread, const EeWaitState &wait) noexcept
        {
            if (wait.reason != EeWaitReason::Semaphore && wait.reason != EeWaitReason::External &&
                wait.reason != EeWaitReason::Mpeg) return;
            Active *item = entry(thread.id);
            if (!item) { ++dropped; return; }
            const uint32_t object = wait.reason == EeWaitReason::Semaphore
                ? static_cast<uint32_t>(std::get<EeSemaphoreWait>(wait.payload).id)
                : std::get<EeExternalWait>(wait.payload).type;
            const auto &ctx = thread.activeContext();
            const uint32_t ra = getRegU32(&ctx, 31);
            size_t slot = buckets.size(), freeSlot = buckets.size();
            for (size_t i = 0; i < buckets.size(); ++i)
            {
                const auto &b = buckets[i];
                if (!b.used) { if (freeSlot == buckets.size()) freeSlot = i; continue; }
                if (b.thread == thread.id && b.reason == wait.reason && b.object == object &&
                    b.pc == ctx.pc && b.ra == ra) { slot = i; break; }
            }
            if (slot == buckets.size()) slot = freeSlot;
            if (slot == buckets.size()) { ++dropped; return; }
            if (item->phase) ++buckets[item->bucket].abandoned;
            auto &b = buckets[slot];
            b.used = true; b.reason = wait.reason; b.thread = thread.id;
            b.object = object; b.pc = ctx.pc; b.ra = ra; ++b.started;
            *item = {1u, static_cast<unsigned>(slot), thread.invocations.size(), Clock::now(), {}};
            if (windowStart == Clock::time_point{}) windowStart = item->blockedAt;
        }
        void release(int id, int result) noexcept
        {
            Active *item = entry(id);
            if (!item || item->phase != 1u) return;
            auto &b = buckets[item->bucket];
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - item->blockedAt).count();
            ++b.released; if (result < 0) ++b.errors;
            b.blockedMs += ms; b.blockedMaxMs = std::max(b.blockedMaxMs, ms);
            item->phase = 2u;
        }
        void ready(int id) noexcept
        {
            Active *item = entry(id);
            if (item && item->phase == 2u) { item->readyAt = Clock::now(); item->phase = 3u; }
        }
        void resume(const GuestThread &thread) noexcept
        {
            Active *item = entry(thread.id);
            if (!item || item->phase != 3u || item->invocationDepth != thread.invocations.size()) return;
            auto &b = buckets[item->bucket];
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - item->readyAt).count();
            ++b.resumed; b.readyMs += ms; b.readyMaxMs = std::max(b.readyMaxMs, ms);
            *item = {};
        }
        void abandon(int id) noexcept
        {
            Active *item = entry(id);
            if (item && item->phase) { ++buckets[item->bucket].abandoned; *item = {}; }
        }
        void report() noexcept
        {
            if (windowStart == Clock::time_point{}) return;
            const auto now = Clock::now();
            const double seconds = std::chrono::duration<double>(now - windowStart).count();
            if (seconds < 5.0) return;
            for (size_t i = 0; i < buckets.size() && lines < 1200u; ++i)
            {
                auto &b = buckets[i];
                if (!b.used) continue;
                unsigned pending = 0;
                double oldestMs = 0;
                for (const auto &a : active)
                    if (a.phase && a.bucket == i)
                    { ++pending; oldestMs = std::max(oldestMs, std::chrono::duration<double, std::milli>(now - a.blockedAt).count()); }
                if (!(b.started || b.released || b.resumed || b.abandoned || pending)) continue;
                const char *kind = b.reason == EeWaitReason::Semaphore ? "sema" :
                    b.reason == EeWaitReason::Mpeg ? "mpeg" : b.object == 0x52504344u ? "rpc" : "external";
                std::fprintf(stderr, "[perf:wait] interval=%.3fs kind=%s thread=%d object=%08X pc=%08X ra=%08X "
                    "started=%llu released=%llu resumed=%llu errors=%llu abandoned=%llu "
                    "blocked_ms[total=%.3f avg=%.3f max=%.3f] ready_ms[total=%.3f avg=%.3f max=%.3f] "
                    "pending=%u oldest_ms=%.3f dropped=%llu\n", seconds, kind, b.thread, b.object, b.pc, b.ra,
                    static_cast<unsigned long long>(b.started), static_cast<unsigned long long>(b.released),
                    static_cast<unsigned long long>(b.resumed), static_cast<unsigned long long>(b.errors),
                    static_cast<unsigned long long>(b.abandoned), b.blockedMs, b.released ? b.blockedMs / b.released : 0,
                    b.blockedMaxMs, b.readyMs, b.resumed ? b.readyMs / b.resumed : 0, b.readyMaxMs,
                    pending, oldestMs, static_cast<unsigned long long>(dropped));
                ++lines;
                b.started = b.released = b.resumed = b.errors = b.abandoned = 0;
                b.blockedMs = b.blockedMaxMs = b.readyMs = b.readyMaxMs = 0;
                if (!pending) b = {}; // Reuse bounded keys once no wait references them.
            }
            windowStart = now; dropped = 0;
        }
    };

    DodWaitPerf *dodWaitPerf(const EeScheduler *owner) noexcept
    {
        static const bool enabled = std::getenv("DOD_WAIT_TRACE") != nullptr;
        if (!enabled) return nullptr;
        static thread_local DodWaitPerf state;
        if (state.owner != owner) { state = {}; state.owner = owner; }
        return state.lines < 1200u ? &state : nullptr;
    }

    // [DoD] Opt-in EE-thread timing only. A VBlank is a clock event, not proof
    // that the guest submitted a new frame. Keep this separate from host FPS.
    struct DodVBlankPerf
    {
        using Clock = std::chrono::steady_clock;
        Clock::time_point windowStart{};
        Clock::time_point previousDispatch{};
        uint64_t vblanks = 0, intervals = 0, shortIntervals = 0, longIntervals = 0;
        uint64_t lateOver2Ms = 0, lateOver16Ms = 0;
        uint64_t batches = 0, multiBatches = 0, maxBatch = 0;
        double intervalTotalMs = 0.0, intervalMinMs = std::numeric_limits<double>::max(), intervalMaxMs = 0.0;
        double latenessTotalMs = 0.0, latenessMaxMs = 0.0;

        void dispatch(Clock::time_point deadline) noexcept
        {
            const auto now = Clock::now();
            if (windowStart == Clock::time_point{}) windowStart = now;
            if (previousDispatch != Clock::time_point{})
            {
                const double ms = std::chrono::duration<double, std::milli>(now - previousDispatch).count();
                ++intervals;
                intervalTotalMs += ms;
                intervalMinMs = std::min(intervalMinMs, ms);
                intervalMaxMs = std::max(intervalMaxMs, ms);
                if (ms < 8.0) ++shortIntervals;
                if (ms > 25.0) ++longIntervals;
            }
            previousDispatch = now;
            const double lateMs = std::max(0.0, std::chrono::duration<double, std::milli>(now - deadline).count());
            latenessTotalMs += lateMs;
            latenessMaxMs = std::max(latenessMaxMs, lateMs);
            if (lateMs > 2.0) ++lateOver2Ms;
            if (lateMs > 16.667) ++lateOver16Ms;
            ++vblanks;
        }

        void finishBatch(uint64_t count) noexcept
        {
            if (count == 0u) return;
            ++batches;
            if (count > 1u) ++multiBatches;
            maxBatch = std::max(maxBatch, count);
            const auto now = Clock::now();
            const double seconds = std::chrono::duration<double>(now - windowStart).count();
            if (seconds < 5.0) return;
            std::fprintf(stderr,
                         "[perf:guest] interval=%.3fs vblank_n=%llu hz=%.2f "
                         "dispatch_ms[avg=%.3f min=%.3f max=%.3f short_lt8=%llu long_gt25=%llu] "
                         "late_ms[avg=%.3f max=%.3f gt2=%llu gt16_667=%llu] "
                         "dispatch_batches=%llu multi_batches=%llu batch_max=%llu\n",
                         seconds, static_cast<unsigned long long>(vblanks), vblanks / seconds,
                         intervals ? intervalTotalMs / intervals : 0.0,
                         intervals ? intervalMinMs : 0.0, intervalMaxMs,
                         static_cast<unsigned long long>(shortIntervals), static_cast<unsigned long long>(longIntervals),
                         vblanks ? latenessTotalMs / vblanks : 0.0, latenessMaxMs,
                         static_cast<unsigned long long>(lateOver2Ms), static_cast<unsigned long long>(lateOver16Ms),
                         static_cast<unsigned long long>(batches), static_cast<unsigned long long>(multiBatches),
                         static_cast<unsigned long long>(maxBatch));
            windowStart = now;
            vblanks = intervals = shortIntervals = longIntervals = 0;
            lateOver2Ms = lateOver16Ms = batches = multiBatches = maxBatch = 0;
            intervalTotalMs = intervalMaxMs = latenessTotalMs = latenessMaxMs = 0.0;
            intervalMinMs = std::numeric_limits<double>::max();
        }
    };

    struct DodVBlankPerfBatch
    {
        DodVBlankPerf *perf = nullptr;
        uint64_t count = 0;

        DodVBlankPerfBatch() noexcept
        {
            static const bool enabled = std::getenv("DOD_PERF") != nullptr;
            if (enabled)
            {
                static thread_local DodVBlankPerf state;
                perf = &state;
            }
        }
        ~DodVBlankPerfBatch() { if (perf) perf->finishBatch(count); }
        void dispatch(DodVBlankPerf::Clock::time_point deadline) noexcept
        {
            if (!perf) return;
            ++count;
            perf->dispatch(deadline);
        }
    };

    constexpr int KE_OK = 0;
    constexpr int KE_ERROR = -1;
    constexpr int KE_ILLEGAL_PRIORITY = -403;
    constexpr int KE_ILLEGAL_THID = -406;
    constexpr int KE_UNKNOWN_THID = -407;
    constexpr int KE_UNKNOWN_SEMID = -408;
    constexpr int KE_UNKNOWN_EVFID = -409;
    constexpr int KE_DORMANT = -413;
    constexpr int KE_NOT_DORMANT = -414;
    constexpr int KE_NOT_SUSPEND = -415;
    constexpr int KE_NOT_WAIT = -416;
    constexpr int KE_RELEASE_WAIT = -418;
    constexpr int KE_SEMA_ZERO = -419;
    constexpr int KE_SEMA_OVF = -420;
    constexpr int KE_EVF_COND = -421;
    constexpr int KE_WAIT_DELETE = -425;

    constexpr uint32_t WEF_OR = 0x01u;
    constexpr uint32_t WEF_CLEAR = 0x10u;
    constexpr uint32_t WEF_CLEAR_ALL = 0x20u;
    // [DoD] The NTSC field is 60/1.001 Hz (16683.35 us), not 60 Hz (GoW port #110: movies ran at 30.00 instead of 29.97
    // pictures/s and their sound fell behind by 0.1 %).
    constexpr auto kVBlankPeriod = std::chrono::nanoseconds(16683350);
    constexpr auto kVBlankDuration = std::chrono::microseconds(500);
    constexpr auto kMaxVBlankPacingDebt = std::chrono::milliseconds(100); // ran-j/PS2Recomp#332
    constexpr uint64_t kAlarmTickMicroseconds = 64u;
    constexpr uint32_t kDebugPublishDispatchInterval = 4096u;

    constexpr uint64_t microsecondsToEeCycles(uint64_t microseconds)
    {
        return (microseconds * EeScheduler::kEeClockHz + 999999ull) / 1000000ull;
    }

    std::chrono::nanoseconds eeCyclesToHostDuration(uint64_t cycles)
    {
        constexpr uint64_t kNanosecondsPerSecond = 1000000000ull;
        const uint64_t wholeSeconds = cycles / EeScheduler::kEeClockHz;
        const uint64_t remainingCycles = cycles % EeScheduler::kEeClockHz;
        const uint64_t remainingNanoseconds = (remainingCycles * kNanosecondsPerSecond + EeScheduler::kEeClockHz - 1u) / EeScheduler::kEeClockHz;
        return std::chrono::seconds(wholeSeconds) + std::chrono::nanoseconds(remainingNanoseconds);
    }

    constexpr uint64_t kVBlankPeriodCycles = EeScheduler::kEeClockHz * 1001ull / 60000ull;
    constexpr uint64_t kVBlankDurationCycles = microsecondsToEeCycles(500u);
    constexpr uint64_t kAlarmTickCycles = microsecondsToEeCycles(kAlarmTickMicroseconds);

    template <typename Map>
    int allocatePositiveId(int &nextId, const Map &objects)
    {
        const int first = std::max(1, nextId);
        int candidate = first;
        do
        {
            if (!objects.contains(candidate))
            {
                nextId = (candidate == std::numeric_limits<int>::max()) ? 1 : candidate + 1;
                return candidate;
            }
            candidate = (candidate == std::numeric_limits<int>::max()) ? 1 : candidate + 1;
        } while (candidate != first);
        return 0;
    }
}

EeScheduler::EeScheduler(PS2Runtime &runtime)
    : m_runtime(runtime)
{
}

EeScheduler::~EeScheduler()
{
    requestStop();
}

void EeScheduler::reset(uint8_t *rdram, const R5900Context &mainContext)
{
    if (auto *perf = dodWaitPerf(this)) { *perf = {}; perf->owner = this; }
    m_executorThread = std::this_thread::get_id();
    m_rdram = rdram;
    m_readyQueues = {};
    m_threads.clear();
    m_semaphores.clear();
    m_eventFlags.clear();
    m_alarms.clear();
    m_intcHandlers.clear();
    m_dmacHandlers.clear();
    m_nextThreadId = kFirstThreadId;
    m_nextInvocationThreadId = -1;
    m_nextSemaphoreId = 1;
    m_nextEventFlagId = 1;
    m_nextAlarmId = 1;
    m_nextIntcHandlerId = 1;
    m_nextDmacHandlerId = 1;
    m_intcHeadOrder = 0;
    m_intcTailOrder = 1000;
    m_dmacHeadOrder = 0;
    m_dmacTailOrder = 1000;
    m_enabledIntcMask = 0xFFFFFFFFu;
    m_enabledDmacMask = 0xFFFFFFFFu;
    g_dodPendingMaskedIntc = 0u;
    g_dodPendingMaskedDmac = 0u;
    m_currentThreadId = 0;
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
    m_insideInterrupt = false;
    m_pendingEeTimerInterrupts = 0u;
    m_eeCycle = 0u;
    m_sliceEndCycle = kDefaultTimeSliceCycles;
    m_stopRequested.store(false, std::memory_order_release);
    m_checkpointPending.store(false, std::memory_order_release);
    m_debugPublishCountdown = 0u;
    {
        std::lock_guard lock(m_eventMutex);
        m_events.clear();
        g_dodQueuedEvents.store(0u, std::memory_order_seq_cst);
        m_deadlines.clear();
        updateNextDeadline();
        m_pendingInvocations.clear();
    }
    m_eventSequence = 0;
    m_invocationSequence = 0;
    m_vsyncTick = 0;
    m_vsyncFlagAddress = 0;
    m_vsyncTickAddress = 0;
    m_gsVSyncCallback = 0;
    m_gsVSyncCallbackGp = 0;
    m_gsVSyncCallbackSp = 0;
    m_runtime.memory().gs().vsyncTick.store(0u, std::memory_order_release);
    m_runtime.memory().resetEeTimers();

    GuestThread main{};
    main.id = kMainThreadId;
    main.context = mainContext;
    main.entry = mainContext.pc;
    // $sp is live execution state, not the stable initial stack descriptor
    // returned by ReferThreadStatus. SetupThread records that metadata.
    main.stack = 0u;
    main.gp = getRegU32(&mainContext, 28);
    main.initialPriority = 0;
    main.currentPriority = 0;
    main.status = EeThreadStatus::Ready;
    m_threads.emplace(main.id, std::move(main));
    m_readyQueues[0].push_back(kMainThreadId);
    scheduleEvent(m_eeCycle + kVBlankPeriodCycles,
                  std::chrono::steady_clock::now() + dodVBlankHostPeriod(),
                  EeEvent{EeEventType::VBlankStart, 0, 0});
    publishSnapshot();
}

void EeScheduler::run()
{
    assertExecutor();
    m_running.store(true, std::memory_order_release);

    while (!m_stopRequested.load(std::memory_order_acquire))
    {
        if (dodDispatchStatsOn() && (++g_dodDispatch.loops & 255u) == 0u)
            dodDispatchStatsPrint(m_eeCycle);
        processPendingEvents();
        if (m_stopRequested.load(std::memory_order_acquire))
        {
            break;
        }

        if (m_currentThreadId == 0)
        {
            GuestThread *next = selectReady();
            if (!next && m_pendingInvocations.empty())
            {
                copyMainContextToRuntime();
                publishIdleDebugContext();
                publishSnapshot();
                if (dodDispatchStatsOn())
                    ++g_dodDispatch.idleWaits;
                waitForEvent();
                continue;
            }
            if (next)
            {
                makeRunning(*next);
            }
            else
            {
                GuestThread *owner = &acquireInvocationThread();
                GuestInvocation invocation = std::move(m_pendingInvocations.front());
                m_pendingInvocations.pop_front();
                owner->status = EeThreadStatus::Running;
                m_currentThreadId = owner->id;
                renewTimeSlice();
                if (getRegU32(&invocation.context, 29) == 0u)
                {
                    SET_GPR_U32(&invocation.context, 29, invocationStackTop());
                }
                owner->invocations.push_back(std::move(invocation));
            }
        }

        GuestThread *running = currentThread();
        assert(running != nullptr);
        if (running->resumeCompletion)
        {
            if (auto *perf = dodWaitPerf(this)) perf->resume(*running);
            if (dodDispatchStatsOn())
                ++g_dodDispatch.completions;
            auto completion = std::move(running->resumeCompletion);
            running->resumeCompletion = {};
            try
            {
                g_ps2GuestUnwinding = false;
                completion(running->activeContext());
            }
            catch (const EeDispatcherTransfer &)
            {
            }
            if (m_currentThreadId == 0)
            {
                continue;
            }
        }
        R5900Context &context = running->activeContext();
        if (m_debugPublishCountdown == 0u)
        {
            copyMainContextToRuntime();
            publishSnapshot();
            m_debugPublishCountdown = kDebugPublishDispatchInterval - 1u;
        }
        else
        {
            --m_debugPublishCountdown;
        }

        publishDebugContext(context);

        if (context.pc == 0u)
        {
            if (!running->invocations.empty())
            {
                GuestInvocation completed = std::move(running->invocations.back());
                running->invocations.pop_back();
                if (completed.onComplete)
                {
                    try
                    {
                        completed.onComplete(completed.context, running->activeContext());
                    }
                    catch (const EeDispatcherTransfer &)
                    {
                    }
                }
                continue;
            }
            // Negative ids are synthetic callback carriers. Their zero base
            // context is expected after the last invocation has been popped.
            if (running->id >= 0)
            {
                std::cerr << "[ee] thread " << running->id << " returned to pc 0 (entry=0x" << std::hex << running->entry
                          << " sp=0x" << getRegU32(&context, 29) << std::dec << ")" << std::endl;
            }
#if defined(DOD_FUNCTION_RING)
            if (running->id == kMainThreadId)
            {
                std::string last;
                const uint32_t end = dod_ring::g_next;
                for (uint32_t i = end - std::min<uint32_t>(end, 80u); i != end; ++i)
                {
                    const char *n = dod_ring::g_names[i & (dod_ring::kSize - 1u)];
                    last += n ? n : "?";
                    last += ' ';
                }
                std::cerr << "[ee]   last calls: " << last << std::endl;
            }
#endif
            makeDormant(*running);
            m_currentThreadId = 0;
            copyMainContextToRuntime();
            publishIdleDebugContext();
            publishSnapshot();
            continue;
        }

        g_dodInvocationsHeld = false;
        if (!m_pendingInvocations.empty() && running->invocations.empty() && !dodGuestInterruptsEnabled(context))
            g_dodInvocationsHeld = true;
        else if (!m_pendingInvocations.empty() && running->invocations.empty())
        {
            GuestInvocation invocation = std::move(m_pendingInvocations.front());
            m_pendingInvocations.pop_front();
            if (getRegU32(&invocation.context, 29) == 0u)
            {
                SET_GPR_U32(&invocation.context, 29, invocationStackTop());
            }
            running->invocations.push_back(std::move(invocation));
            continue;
        }

        if (!m_runtime.hasFunction(context.pc))
        {
            if (!running->invocations.empty())
            {
                context.pc = 0u;
            }
            else
            {
                m_runtime.reportMissingFunction(m_rdram,
                                                &context,
                                                context.pc,
                                                context.pc,
                                                PS2Runtime::GuestBranchKind::DirectJump,
                                                "EE scheduler");
                makeDormant(*running);
                m_currentThreadId = 0;
            }
            continue;
        }
        PS2Runtime::RecompiledFunction function = m_runtime.lookupFunction(context.pc);

        if (checkpointDue(kGuestDispatchCycles))
        {
            if (dodDispatchStatsOn())
                ++g_dodDispatch.preRunCheckpoint;
            continue;
        }
        if (dodDispatchStatsOn())
        {
            ++g_dodDispatch.guestRuns;
            ++g_dodDispatch.threadRuns[(running->id >= 0 && running->id < 63) ? running->id : 63];
            ++g_dodDispatch.runPc[context.pc];
        }

        try
        {
            if (auto *perf = dodWaitPerf(this)) perf->resume(*running);
            m_insideInterrupt = !running->invocations.empty() && running->invocations.back().kind == GuestInvocationKind::Interrupt;
            m_guestExecuting.store(true, std::memory_order_release);
            g_ps2GuestUnwinding = false;
            dodVu0Load(context);
            g_dodCurrentThreadId = running->invocations.empty() ? running->id : 1000 + running->id;
            function(m_rdram, &context, &m_runtime);
            if (dodDispatchStatsOn())
                ++g_dodDispatch.exitPc[static_cast<uint64_t>(context.pc) | (g_ps2GuestUnwinding ? (1ull << 32) : 0ull)];
            // [DoD] Fast hop (RUNTIME_PATCHES row 122). Guest code that ends by branching out of its recompiled
            // function (a jump-table fragment going to the next one) comes back here with ctx->pc set, not unwinding.
            // While nothing waits for the dispatcher (no event or deadline: checkpointDue, no thread switch or
            // reschedule, no queued invocation or completion, the same invocation level), start the next function
            // here: the same checkpointDue per hop as the full loop below, without its two event passes, debug
            // publishing and VU0 save/load. DoD's decompressor loops (0x18D938.., 0x153D80) hopped 8 million times a
            // second while reading IMAGE.AFS, and the game dropped to 44 fps. DOD_FAST_HOP=0 turns it off.
            if (dodFastHopOn())
            {
                const int hopThread = m_currentThreadId;
                const size_t hopDepth = running->invocations.size();
                while (!g_ps2GuestUnwinding && m_currentThreadId == hopThread && !m_rescheduleRequested &&
                       context.pc != 0u && running->invocations.size() == hopDepth && m_pendingInvocations.empty() &&
                       !running->resumeCompletion && !m_checkpointPending.load(std::memory_order_acquire) &&
                       !m_stopRequested.load(std::memory_order_acquire) && m_runtime.hasFunction(context.pc))
                {
                    if (checkpointDue(kGuestDispatchCycles))
                    {
                        if (dodDispatchStatsOn())
                            ++g_dodDispatch.preRunCheckpoint;
                        break; // the loop below handles it, then checks again before the next run (as before)
                    }
                    PS2Runtime::RecompiledFunction next = m_runtime.lookupFunction(context.pc);
                    if (dodDispatchStatsOn())
                    {
                        ++g_dodDispatch.hops;
                        ++g_dodDispatch.runPc[context.pc];
                    }
                    next(m_rdram, &context, &m_runtime);
                    if (dodDispatchStatsOn())
                        ++g_dodDispatch.exitPc[static_cast<uint64_t>(context.pc) | (g_ps2GuestUnwinding ? (1ull << 32) : 0ull)];
                }
            }
            dodVu0Save(context);
            m_guestExecuting.store(false, std::memory_order_release);
            m_insideInterrupt = false;
        }
        catch (const EeDispatcherTransfer &)
        {
            dodVu0Save(context);
            m_guestExecuting.store(false, std::memory_order_release);
            m_insideInterrupt = false;
        }
        catch (...)
        {
            m_guestExecuting.store(false, std::memory_order_release);
            m_running.store(false, std::memory_order_release);
            publishSnapshot();
            throw;
        }

        processPendingEvents();
        if (m_rescheduleRequested && m_currentThreadId != 0)
        {
            GuestThread *preempted = currentThread();
            assert(preempted != nullptr);
            enqueueReady(*preempted, !m_timeSliceExpired);
            m_currentThreadId = 0;
            m_rescheduleRequested = false;
            m_timeSliceExpired = false;
        }
    }

    m_guestExecuting.store(false, std::memory_order_release);
    m_running.store(false, std::memory_order_release);
    copyMainContextToRuntime();
    publishSnapshot();
}

void EeScheduler::requestStop()
{
    m_stopRequested.store(true, std::memory_order_release);
    m_checkpointPending.store(true, std::memory_order_release);
    m_eventCv.notify_all();
}

void EeScheduler::postEvent(EeEvent event)
{
    if (event.type == EeEventType::Stop)
    {
        requestStop();
        return;
    }

    if (dodDispatchStatsOn())
        g_dodDispatch.posted[static_cast<size_t>(event.type) & 7u].fetch_add(1u, std::memory_order_relaxed);
    {
        std::lock_guard lock(m_eventMutex);
        m_events.push_back(event);
        g_dodQueuedEvents.fetch_add(1u, std::memory_order_seq_cst);
        m_checkpointPending.store(true, std::memory_order_seq_cst);
    }
    m_eventCv.notify_one();
}

bool EeScheduler::checkpointDue(uint32_t cycles) noexcept
{
    accountCycles(cycles);
    if (m_syncInterruptDepth != 0u)
    {
        return false; // [DoD] runIntcHandlersNow: no preemption inside the nested handler
    }

    if (m_checkpointPending.load(std::memory_order_acquire) ||
        m_stopRequested.load(std::memory_order_acquire))
    {
        if (dodDispatchStatsOn())
            ++(m_stopRequested.load(std::memory_order_acquire) ? g_dodDispatch.cpStop : g_dodDispatch.cpFlag);
        return true;
    }

    const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
    if (nextEventCycle != 0u && m_eeCycle >= nextEventCycle)
    {
        if (dodDispatchStatsOn())
            ++g_dodDispatch.cpDeadline;
        m_checkpointPending.store(true, std::memory_order_release);
        return true;
    }

    if (g_dodInvocationsHeld)
    {
        const GuestThread *running = currentThread();
        if (running == nullptr || dodGuestInterruptsEnabled(running->activeContext()))
        {
            if (dodDispatchStatsOn())
                ++g_dodDispatch.cpHeld;
            return true; // [DoD] EI: deliver the held invocation now
        }
    }

    if (m_eeCycle < m_sliceEndCycle)
    {
        return false;
    }

    const GuestThread *running = currentThread();
    if (running != nullptr && !dodGuestInterruptsEnabled(running->activeContext()))
    {
        return false; // [DoD] DI: keep the slice expired, preempt after EI (#222)
    }
    if (running != nullptr && hasReadyAtOrAbovePriority(running->currentPriority))
    {
        if (dodDispatchStatsOn())
            ++g_dodDispatch.cpSlice;
        m_rescheduleRequested = true;
        m_timeSliceExpired = true;
        return true;
    }

    renewTimeSlice();
    return false;
}

void EeScheduler::accountCycles(uint32_t cycles) noexcept
{
    // [DoD] DOD_EE_CYCLE_SCALE=<percent>: scale the cycles generated code reports (32 per backward branch, 8 per call,
    // whatever the block length) - like PCSX2's EE cycle rate setting.
    static const uint32_t scale = []() -> uint32_t {
        const char *v = std::getenv("DOD_EE_CYCLE_SCALE");
        const long s = v ? std::atol(v) : 100;
        return static_cast<uint32_t>(std::clamp<long>(s, 5, 400));
    }();
    if (scale != 100u)
    {
        static uint32_t remainder = 0;
        const uint32_t scaled = cycles * scale + remainder;
        cycles = scaled / 100u;
        remainder = scaled % 100u;
        if (cycles == 0u)
            return;
    }
    uint64_t elapsed = std::max<uint64_t>(1u, cycles);
    // [DoD] Wall-clock catch-up. Events (VSync, alarms) fire when the EE cycle count reaches them, and the count only
    // grows with the cycles generated code reports; kernel calls and thread switches report almost none. A guest loop
    // that mostly switches threads therefore ran at ~2 VSyncs a second (Dawn of Dreams' loader poll). When the host
    // clock is well past the next event and the EE count still lags, move the count up to the event.
    if (++m_hostCatchUpCountdown >= 1024u)
    {
        m_hostCatchUpCountdown = 0;
        const uint64_t nextCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
        if (nextCycle != 0u && m_eeCycle + elapsed < nextCycle &&
            m_nextDeadlineHost != std::chrono::steady_clock::time_point{} &&
            std::chrono::steady_clock::now() >= m_nextDeadlineHost + std::chrono::milliseconds(2))
        {
            elapsed = nextCycle - m_eeCycle;
        }
    }
    m_eeCycle += elapsed;
    m_pendingEeTimerInterrupts |= m_runtime.memory().advanceEeTimers(elapsed);
    m_runtime.advanceIopEeCycles(elapsed);
    if (m_pendingEeTimerInterrupts != 0u)
    {
        if (dodDispatchStatsOn())
            ++g_dodDispatch.setTimerIrq;
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

bool EeScheduler::isExecutingGuest() const noexcept
{
    return m_guestExecuting.load(std::memory_order_acquire);
}

void EeScheduler::setupCurrentThread(uint32_t stack, uint32_t stackSize, uint32_t gp)
{
    assertExecutor();
    GuestThread *target = currentThread();
    if (!target)
    {
        return;
    }

    target->stack = stack;
    target->stackSize = stackSize;
    target->gp = gp;
    publishSnapshot();
}

int EeScheduler::createThread(const EeThreadCreateParams &params)
{
    assertExecutor();
    // [DoD] 0..127 like the EE kernel: the main thread itself runs at priority 0, and CRI's ADXPS2_SetupThrd
    // creates its ADX threads at the priority ReferThreadStatus reports for the caller (0 in Dawn of Dreams).
    if (params.priority < 0 || params.priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }

    const int id = allocateThreadId();
    if (id == 0)
    {
        return KE_ERROR;
    }

    GuestThread thread{};
    thread.id = id;
    thread.entry = params.entry;
    thread.stack = params.stack;
    thread.stackSize = params.stackSize;
    thread.gp = params.gp;
    thread.attr = params.attr;
    thread.option = params.option;
    thread.initialPriority = params.priority;
    thread.currentPriority = params.priority;
    thread.status = EeThreadStatus::Dormant;
    m_threads.emplace(id, std::move(thread));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteThread(int id, uint32_t &ownedStack)
{
    assertExecutor();
    ownedStack = 0;
    if (id <= kMainThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    auto it = m_threads.find(id);
    if (it == m_threads.end())
    {
        return KE_UNKNOWN_THID;
    }
    if (it->second.status != EeThreadStatus::Dormant)
    {
        return KE_NOT_DORMANT;
    }
    if (it->second.ownsStack)
    {
        ownedStack = it->second.stack;
    }
    m_threads.erase(it);
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::startThread(int id, uint32_t arg, const R5900Context &caller, bool interruptSafe)
{
    assertExecutor();
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status != EeThreadStatus::Dormant)
    {
        return KE_NOT_DORMANT;
    }

    target->context = R5900Context{};
    target->context.vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f); // [DoD] VU0 vf0 is hardwired to (0,0,0,1)
    target->context.pc = target->entry;
    target->arg = arg;
    target->suspendCount = 0;
    target->wakeupCount = 0;
    target->wait = {};
    SET_GPR_U32(&target->context, 4, arg);
    SET_GPR_U32(&target->context, 28, target->gp != 0u ? target->gp : getRegU32(&caller, 28));
    const uint32_t stackTop = target->stack != 0u
                                  ? (target->stack + target->stackSize) & ~0xFu
                                  : getRegU32(&caller, 29);
    SET_GPR_U32(&target->context, 29, stackTop);
    SET_GPR_U32(&target->context, 31, 0u);
    enqueueReady(*target);
    requestPreemptionIfHigher(*target, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

[[noreturn]] void EeScheduler::exitCurrent(bool deleteThreadRecord)
{
    assertExecutor();
    GuestThread *exiting = currentThread();
    assert(exiting != nullptr);
    const int id = exiting->id;
    const uint32_t ownedStack = deleteThreadRecord && exiting->ownsStack ? exiting->stack : 0u;
    // [DoD] diagnostics: which guest thread ends, and from where
    std::cerr << "[ee] thread " << id << " exits (entry=0x" << std::hex << exiting->entry << " pc=0x" << exiting->context.pc
              << " ra=0x" << getRegU32(&exiting->context, 31) << std::dec << (deleteThreadRecord ? ", delete" : "") << ")" << std::endl;
    makeDormant(*exiting);
    m_currentThreadId = 0;
    if (deleteThreadRecord && id != kMainThreadId)
    {
        m_threads.erase(id);
    }
    if (ownedStack != 0u)
    {
        m_runtime.guestFree(ownedStack);
    }
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

int EeScheduler::terminateThread(int id, uint32_t &ownedStack, bool interruptSafe)
{
    assertExecutor();
    ownedStack = 0;
    if (id == 0 || id == m_currentThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }
    if (target->ownsStack)
    {
        ownedStack = target->stack;
        target->ownsStack = false;
    }
    std::cerr << "[ee] thread " << id << " terminated by thread " << m_currentThreadId << " (entry=0x" << std::hex
              << target->entry << " pc=0x" << target->context.pc << std::dec << ")" << std::endl; // [DoD] diagnostics
    makeDormant(*target);
    (void)interruptSafe;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::suspendThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }

    ++target->suspendCount;
    switch (target->status)
    {
    case EeThreadStatus::Running:
        target->status = EeThreadStatus::Suspended;
        m_currentThreadId = 0;
        m_rescheduleRequested = true;
        break;
    case EeThreadStatus::Ready:
        removeReady(*target);
        target->status = EeThreadStatus::Suspended;
        break;
    case EeThreadStatus::Waiting:
        target->status = EeThreadStatus::WaitingSuspended;
        break;
    case EeThreadStatus::WaitingSuspended:
    case EeThreadStatus::Suspended:
        break;
    case EeThreadStatus::Dormant:
        break;
    }
    if (interruptSafe && m_insideInterrupt)
    {
        m_rescheduleRequested = true;
    }
    publishSnapshot();
    // [DoD] The EE kernel's (i)SuspendThread returns the thread id on success, like ResumeThread (Urban Reign port:
    // CRI's ADXM server compares the results; with 0 its movie stop handshake could be lost and the game slept forever
    // after a skipped movie).
    return id;
}

int EeScheduler::resumeThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->suspendCount == 0)
    {
        return KE_NOT_SUSPEND;
    }
    // EE ResumeThread returns the target id on success. CRI compares it
    // with that id before WakeupThread for a sleeping suspended worker.
    --target->suspendCount;
    if (target->suspendCount != 0)
    {
        return id;
    }
    if (target->status == EeThreadStatus::WaitingSuspended)
    {
        target->status = EeThreadStatus::Waiting;
    }
    else if (target->status == EeThreadStatus::Suspended)
    {
        enqueueReady(*target);
        requestPreemptionIfHigher(*target, interruptSafe);
    }
    publishSnapshot();
    return id;
}

void EeScheduler::sleepCurrent()
{
    assertExecutor();
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (self->wakeupCount != 0u)
    {
        --self->wakeupCount;
        setReturnS32(&self->activeContext(), KE_OK);
        return;
    }
    blockCurrent(EeWaitState{EeWaitReason::Sleep, std::monostate{}});
}

int EeScheduler::wakeupThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0 || id == m_currentThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }
    if ((target->status == EeThreadStatus::Waiting || target->status == EeThreadStatus::WaitingSuspended) &&
        target->wait.reason == EeWaitReason::Sleep)
    {
        makeReady(*target, KE_OK, interruptSafe);
    }
    else
    {
        ++target->wakeupCount;
    }
    publishSnapshot();
    return id; // [DoD] (i)WakeupThread returns the thread id on success (EE kernel; Urban Reign port)
}

int EeScheduler::cancelWakeup(int id)
{
    assertExecutor();
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    const int old = static_cast<int>(target->wakeupCount);
    target->wakeupCount = 0;
    publishSnapshot();
    return old;
}

int EeScheduler::changePriority(int id, int priority, bool interruptSafe, int &oldPriority)
{
    assertExecutor();
    if (priority < 0 || priority >= kPriorityCount) // [DoD] 0..127, see createThread
    {
        return KE_ILLEGAL_PRIORITY;
    }
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    oldPriority = target->currentPriority;
    static int s_logged = 0; // [DoD] diagnostics: the first priority changes
    if (s_logged < 40)
    {
        ++s_logged;
        std::cerr << "[ee] ChangeThreadPriority thread " << id << " " << oldPriority << " -> " << priority
                  << " (by thread " << m_currentThreadId << ")" << std::endl;
    }
    if (target->status == EeThreadStatus::Ready)
    {
        removeReady(*target);
        target->currentPriority = priority;
        enqueueReady(*target);
        requestPreemptionIfHigher(*target, interruptSafe);
    }
    else
    {
        target->currentPriority = priority;
        if (target->status == EeThreadStatus::Running)
        {
            for (int p = 0; p < target->currentPriority; ++p)
            {
                if (!m_readyQueues[p].empty())
                {
                    m_rescheduleRequested = true;
                    break;
                }
            }
        }
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::rotateReadyQueue(int priority, bool interruptSafe)
{
    assertExecutor();
    if (priority == 0)
    {
        const GuestThread *self = currentThread();
        priority = self ? self->currentPriority : 0;
    }
    if (priority < 0 || priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }

    GuestThread *self = currentThread();
    if (self && self->currentPriority == priority)
    {
        enqueueReady(*self);
        m_currentThreadId = 0;
        m_rescheduleRequested = true;
    }
    else
    {
        auto &queue = m_readyQueues[priority];
        if (queue.size() > 1u)
        {
            const int head = queue.front();
            queue.pop_front();
            queue.push_back(head);
        }
    }
    (void)interruptSafe;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::releaseWait(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status != EeThreadStatus::Waiting && target->status != EeThreadStatus::WaitingSuspended)
    {
        return KE_NOT_WAIT;
    }
    removeFromWaitObject(*target);
    makeReady(*target, KE_RELEASE_WAIT, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::transferIfRequested(bool interruptSafe)
{
    assertExecutor();
    if (interruptSafe || m_insideInterrupt || !m_rescheduleRequested)
    {
        return;
    }
    if (m_currentThreadId != 0)
    {
        GuestThread *self = currentThread();
        assert(self != nullptr);
        enqueueReady(*self, true);
        m_currentThreadId = 0;
    }
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

int EeScheduler::createSemaphore(int initCount, int maxCount, uint32_t attr, uint32_t option)
{
    assertExecutor();
    if (maxCount <= 0 || initCount < 0 || initCount > maxCount)
    {
        return KE_ERROR;
    }
    const int id = allocatePositiveId(m_nextSemaphoreId, m_semaphores);
    if (id == 0)
    {
        return KE_ERROR;
    }
    EeSemaphore semaphore{};
    semaphore.id = id;
    semaphore.count = initCount;
    semaphore.maxCount = maxCount;
    semaphore.initCount = initCount;
    semaphore.attr = attr;
    semaphore.option = option;
    m_semaphores.emplace(id, std::move(semaphore));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteSemaphore(int id, bool interruptSafe)
{
    assertExecutor();
    auto it = m_semaphores.find(id);
    if (it == m_semaphores.end())
    {
        return KE_UNKNOWN_SEMID;
    }
    std::deque<int> waiters = std::move(it->second.waiters);
    m_semaphores.erase(it);
    for (const int threadId : waiters)
    {
        if (GuestThread *waiter = thread(threadId))
        {
            makeReady(*waiter, KE_WAIT_DELETE, interruptSafe);
        }
    }
    publishSnapshot();
    return id;
}

int EeScheduler::signalSemaphore(int id, bool interruptSafe)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        return KE_UNKNOWN_SEMID;
    }
    if (!object->waiters.empty())
    {
        const int waiterId = object->waiters.front();
        object->waiters.pop_front();
        GuestThread *waiter = thread(waiterId);
        assert(waiter != nullptr);
        makeReady(*waiter, id, interruptSafe);
        publishSnapshot();
        return id;
    }
    if (object->count == object->maxCount)
    {
        return KE_SEMA_OVF;
    }
    ++object->count;
    publishSnapshot();
    return id;
}

int EeScheduler::pollSemaphore(int id)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        return KE_UNKNOWN_SEMID;
    }
    if (object->count == 0)
    {
        return KE_SEMA_ZERO;
    }
    --object->count;
    publishSnapshot();
    return id;
}

void EeScheduler::waitSemaphore(int id)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        GuestThread *self = currentThread();
        assert(self != nullptr);
        setReturnS32(&self->activeContext(), KE_UNKNOWN_SEMID);
        return;
    }
    if (object->count != 0)
    {
        --object->count;
        GuestThread *self = currentThread();
        assert(self != nullptr);
        setReturnS32(&self->activeContext(), id);
        publishSnapshot();
        return;
    }
    GuestThread *self = currentThread();
    assert(self != nullptr);
    object->waiters.push_back(self->id);
    blockCurrent(EeWaitState{EeWaitReason::Semaphore, EeSemaphoreWait{id}});
}

int EeScheduler::createEventFlag(uint32_t initialBits, uint32_t attr, uint32_t option)
{
    assertExecutor();
    const int id = allocatePositiveId(m_nextEventFlagId, m_eventFlags);
    if (id == 0)
    {
        return KE_ERROR;
    }
    EeEventFlag flag{};
    flag.id = id;
    flag.attr = attr;
    flag.option = option;
    flag.initBits = initialBits;
    flag.bits = initialBits;
    m_eventFlags.emplace(id, std::move(flag));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteEventFlag(int id, bool interruptSafe)
{
    assertExecutor();
    auto it = m_eventFlags.find(id);
    if (it == m_eventFlags.end())
    {
        return KE_UNKNOWN_EVFID;
    }
    std::deque<int> waiters = std::move(it->second.waiters);
    m_eventFlags.erase(it);
    for (const int threadId : waiters)
    {
        if (GuestThread *waiter = thread(threadId))
        {
            makeReady(*waiter, KE_WAIT_DELETE, interruptSafe);
        }
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::setEventFlag(int id, uint32_t bits, bool interruptSafe)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    flag->bits |= bits;
    finishEventWaiters(*flag, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::clearEventFlag(int id, uint32_t mask)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    flag->bits &= mask;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::pollEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t &observedBits)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    if (!eventCondition(flag->bits, bits, mode))
    {
        return KE_EVF_COND;
    }
    observedBits = flag->bits;
    if ((mode & WEF_CLEAR_ALL) != 0u)
    {
        flag->bits = 0;
    }
    else if ((mode & WEF_CLEAR) != 0u)
    {
        flag->bits &= ~bits;
    }
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::waitEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t resultAddress)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (!flag)
    {
        setReturnS32(&self->activeContext(), KE_UNKNOWN_EVFID);
        return;
    }
    if (eventCondition(flag->bits, bits, mode))
    {
        const uint32_t observed = flag->bits;
        writeGuestU32(resultAddress, observed);
        if ((mode & WEF_CLEAR_ALL) != 0u)
        {
            flag->bits = 0;
        }
        else if ((mode & WEF_CLEAR) != 0u)
        {
            flag->bits &= ~bits;
        }
        setReturnS32(&self->activeContext(), KE_OK);
        publishSnapshot();
        return;
    }
    flag->waiters.push_back(self->id);
    blockCurrent(EeWaitState{EeWaitReason::EventFlag,
                             EeEventFlagWait{id, bits, mode, resultAddress}});
}

int EeScheduler::setAlarm(uint16_t ticks,
                          uint32_t handler,
                          uint32_t argument,
                          uint32_t gp,
                          uint32_t sp)
{
    assertExecutor();
    if (handler == 0u || !m_runtime.hasFunction(handler))
    {
        return KE_ERROR;
    }
    const int id = allocatePositiveId(m_nextAlarmId, m_alarms);
    if (id == 0)
    {
        return KE_ERROR;
    }
    m_alarms.emplace(id, EeAlarm{id, ticks, handler, argument, gp, sp});
    const uint64_t tickCount = ticks == 0u ? 1u : static_cast<uint64_t>(ticks);
    scheduleEvent(m_eeCycle + tickCount * kAlarmTickCycles,
                  std::chrono::steady_clock::now() + std::chrono::microseconds(tickCount * kAlarmTickMicroseconds),
                  EeEvent{EeEventType::Alarm, static_cast<uint32_t>(id), 0});
    return id;
}

int EeScheduler::cancelAlarm(int id)
{
    assertExecutor();
    if (m_alarms.erase(id) == 0u)
    {
        return KE_ERROR;
    }
    {
        std::lock_guard lock(m_eventMutex);
        std::erase_if(m_deadlines, [id](const ScheduledEvent &scheduled)
                      { return scheduled.event.type == EeEventType::Alarm &&
                               scheduled.event.id == static_cast<uint32_t>(id); });
        updateNextDeadline();
    }
    return KE_OK;
}

void EeScheduler::queueInvocation(GuestInvocation invocation)
{
    assertExecutor();
    invocation.sequence = ++m_invocationSequence;
    m_pendingInvocations.push_back(std::move(invocation));
    if (dodDispatchStatsOn())
        ++g_dodDispatch.setInvocation;
    m_checkpointPending.store(true, std::memory_order_release);
}

[[noreturn]] void EeScheduler::invokeCurrent(GuestInvocation invocation)
{
    assertExecutor();
    GuestThread *owner = currentThread();
    assert(owner != nullptr);
    if (getRegU32(&invocation.context, 29) == 0u)
    {
        SET_GPR_U32(&invocation.context, 29, invocationStackTop());
    }
    invocation.sequence = ++m_invocationSequence;
    owner->invocations.push_back(std::move(invocation));
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

[[noreturn]] void EeScheduler::invokeCurrentSequence(std::vector<GuestInvocation> invocations)
{
    assertExecutor();
    GuestThread *owner = currentThread();
    assert(owner != nullptr);
    assert(!invocations.empty());
    for (auto it = invocations.rbegin(); it != invocations.rend(); ++it)
    {
        if (getRegU32(&it->context, 29) == 0u)
        {
            SET_GPR_U32(&it->context, 29, invocationStackTop());
        }
        it->sequence = ++m_invocationSequence;
        owner->invocations.push_back(std::move(*it));
    }
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

bool EeScheduler::hasInvocation(GuestInvocationKind kind, uint64_t tag) const
{
    const GuestThread *owner = currentThread();
    if (!owner)
    {
        return false;
    }
    return std::any_of(owner->invocations.begin(), owner->invocations.end(),
                       [kind, tag](const GuestInvocation &invocation)
                       {
                           return invocation.kind == kind && invocation.tag == tag;
                       });
}

uint32_t EeScheduler::invocationStackTop()
{
    assertExecutor();
    const GuestThread *owner = currentThread();
    if (!owner)
    {
        throw std::logic_error("EE invocation stack requested without a current guest context");
    }
    const size_t depth = owner ? owner->invocations.size() : 0u;
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(owner->id)) << 32u) |
                         static_cast<uint32_t>(depth);
    const auto existing = m_invocationStackTops.find(key);
    if (existing != m_invocationStackTops.end())
    {
        return existing->second;
    }
    // [DoD] Each (thread, depth) that ever ran an invocation (an interrupt or alarm handler, an RPC end callback...)
    // used to keep its 16 KB stack forever. The kernel window for them (ps2_runtime.cpp reserveAsyncCallbackStack,
    // 0x80000..0x100000) holds 32; past that the stacks were carved from the top of RAM, where Dawn of Dreams keeps
    // its own thread stacks. God Hand's port (godhand-recomp EeScheduler.cpp) crashed this way after ~30 minutes of
    // play. Stacks whose key is not in use any more are now reused: the free list first, then a reclaim pass, then
    // new space.
    constexpr uint32_t kInvocationStackSize = 0x4000u;
    uint32_t top = 0u;
    if (m_freeInvocationStacks.empty())
        reclaimInvocationStacks();
    if (!m_freeInvocationStacks.empty())
    {
        top = m_freeInvocationStacks.back();
        m_freeInvocationStacks.pop_back();
    }
    else
    {
        top = m_runtime.reserveAsyncCallbackStack(kInvocationStackSize, 16u);
        static uint32_t s_reserved = 0u;
        ++s_reserved;
        if (s_reserved <= 4u || (s_reserved & (s_reserved - 1u)) == 0u)
            std::fprintf(stderr, "[ee] invocation stacks: %u reserved so far, %zu keys in use (new key: thread %d depth %zu)\n",
                         s_reserved, m_invocationStackTops.size() + 1u, owner->id, depth);
    }
    if (top == 0u)
    {
        throw std::runtime_error("EE invocation stack space exhausted");
    }
    m_invocationStackTops.emplace(key, top);
    return top;
}

size_t EeScheduler::reclaimInvocationStacks()
{
    // [DoD] A stack is in use while its thread exists and has an invocation at that depth (index `depth` of
    // invocations, whose $sp came from this key when it was pushed). Deleted threads and finished depths give theirs
    // back (godhand-recomp's reclaim pass).
    size_t reclaimed = 0u;
    for (auto it = m_invocationStackTops.begin(); it != m_invocationStackTops.end();)
    {
        const int id = static_cast<int>(static_cast<uint32_t>(it->first >> 32u));
        const size_t depth = static_cast<uint32_t>(it->first);
        const auto thread = m_threads.find(id);
        if (thread != m_threads.end() && thread->second.invocations.size() > depth)
        {
            ++it;
            continue;
        }
        m_freeInvocationStacks.push_back(it->second);
        it = m_invocationStackTops.erase(it);
        ++reclaimed;
    }
    static uint32_t s_logsLeft = 8u;
    if (reclaimed != 0u && s_logsLeft != 0u)
    {
        --s_logsLeft;
        std::fprintf(stderr, "[ee] invocation stacks: %zu reclaimed from finished threads/depths, %zu still in use\n",
                     reclaimed, m_invocationStackTops.size());
    }
    return reclaimed;
}

int EeScheduler::addIrqHandler(bool dmac,
                               uint32_t cause,
                               uint32_t handler,
                               bool append,
                               uint32_t argument,
                               uint32_t gp,
                               uint32_t sp)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    int &nextId = dmac ? m_nextDmacHandlerId : m_nextIntcHandlerId;
    const int id = allocatePositiveId(nextId, handlers);
    if (id == 0)
    {
        return KE_ERROR;
    }
    int &head = dmac ? m_dmacHeadOrder : m_intcHeadOrder;
    int &tail = dmac ? m_dmacTailOrder : m_intcTailOrder;
    static int s_logged = 0; // [DoD] diagnostics: which interrupt causes the game handles
    if (s_logged < 64)
    {
        ++s_logged;
        std::fprintf(stderr, "[ee] Add%sHandler cause %u handler %08X id %d\n", dmac ? "Dmac" : "Intc", cause, handler, id);
    }
    handlers.emplace(id,
                     EeIrqHandler{id,
                                  cause,
                                  handler,
                                  argument,
                                  gp,
                                  sp,
                                  true,
                                  append ? ++tail : --head});
    return id;
}

int EeScheduler::removeIrqHandler(bool dmac, uint32_t cause, int id)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    auto it = handlers.find(id);
    if (it != handlers.end() && it->second.cause == cause)
    {
        handlers.erase(it);
    }
    return KE_OK;
}

int EeScheduler::setIrqHandlerEnabled(bool dmac, int id, bool enabled)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    auto it = handlers.find(id);
    if (it != handlers.end())
    {
        it->second.enabled = enabled;
    }
    return KE_OK;
}

int EeScheduler::setIrqCauseEnabled(bool dmac, uint32_t cause, bool enabled)
{
    assertExecutor();
    if (cause < 32u)
    {
        uint32_t &mask = dmac ? m_enabledDmacMask : m_enabledIntcMask;
        if (enabled)
        {
            const bool wasDisabled = (mask & (1u << cause)) == 0u;
            mask |= 1u << cause;
            uint32_t &latched = dmac ? g_dodPendingMaskedDmac : g_dodPendingMaskedIntc;
            if (wasDisabled && (latched & (1u << cause)) != 0u)
            {
                latched &= ~(1u << cause);
                dispatchIrq(dmac, cause);
            }
        }
        else
        {
            mask &= ~(1u << cause);
        }
    }
    return KE_OK;
}

void EeScheduler::dispatchIrq(bool dmac, uint32_t cause)
{
    assertExecutor();
    // [DoD] INTC_STAT latches the cause whether or not it is masked (libkernel's VSync wait polls it).
    if (!dmac && cause < 15u)
        dodRaiseIntcStat(m_runtime.memory(), 1u << cause);
    const uint32_t mask = dmac ? m_enabledDmacMask : m_enabledIntcMask;
    if (cause < 32u && (mask & (1u << cause)) == 0u)
    {
        (dmac ? g_dodPendingMaskedDmac : g_dodPendingMaskedIntc) |= 1u << cause; // [DoD] latched, not dropped
        return;
    }
    const auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    std::vector<EeIrqHandler> matching;
    for (const auto &[id, handler] : handlers)
    {
        (void)id;
        if (handler.enabled && handler.cause == cause && handler.handler != 0u &&
            m_runtime.hasFunction(handler.handler))
        {
            matching.push_back(handler);
        }
    }
    std::sort(matching.begin(), matching.end(), [](const EeIrqHandler &left, const EeIrqHandler &right)
              { return left.order < right.order; });
    for (const EeIrqHandler &handler : matching)
    {
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::Interrupt;
        invocation.context.pc = handler.handler;
        SET_GPR_U32(&invocation.context, 4, cause);
        SET_GPR_U32(&invocation.context, 5, handler.argument);
        SET_GPR_U32(&invocation.context, 28, handler.gp);
        SET_GPR_U32(&invocation.context, 29, 0u);
        SET_GPR_U32(&invocation.context, 31, 0u);
        queueInvocation(std::move(invocation));
    }
}

void EeScheduler::runIntcHandlersNow(uint32_t cause)
{
    if (m_executorThread != std::this_thread::get_id())
    {
        return;
    }
    if (cause < 32u && (m_enabledIntcMask & (1u << cause)) == 0u)
    {
        return;
    }
    std::vector<EeIrqHandler> matching;
    for (const auto &[id, handler] : m_intcHandlers)
    {
        (void)id;
        if (handler.enabled && handler.cause == cause && handler.handler != 0u &&
            m_runtime.hasFunction(handler.handler))
        {
            matching.push_back(handler);
        }
    }
    if (matching.empty())
    {
        return;
    }
    std::sort(matching.begin(), matching.end(), [](const EeIrqHandler &left, const EeIrqHandler &right)
              { return left.order < right.order; });
    if (m_syncInterruptStack == 0u)
    {
        m_syncInterruptStack = m_runtime.reserveAsyncCallbackStack(0x8000u, 16u);
    }

    R5900Context base{};
    if (GuestThread *running = currentThread())
    {
        base = running->activeContext();
    }
    struct DepthGuard
    {
        EeScheduler &scheduler;
        bool wasInsideInterrupt;
        bool wasUnwinding; // [DoD] the interrupted guest code's unwind state
        ~DepthGuard()
        {
            scheduler.m_insideInterrupt = wasInsideInterrupt;
            --scheduler.m_syncInterruptDepth;
            g_ps2GuestUnwinding = wasUnwinding;
        }
    } depthGuard{*this, m_insideInterrupt, g_ps2GuestUnwinding};
    ++m_syncInterruptDepth;
    for (const EeIrqHandler &handler : matching)
    {
        R5900Context context = base;
        context.pc = handler.handler;
        SET_GPR_U32(&context, 4, cause);
        SET_GPR_U32(&context, 5, handler.argument);
        SET_GPR_U32(&context, 28, handler.gp);
        SET_GPR_U32(&context, 29, m_syncInterruptStack - 0x40u * m_syncInterruptDepth);
        SET_GPR_U32(&context, 31, 0u);
        m_insideInterrupt = true;
        static uint32_t s_dodLogged = 0u; // [DoD] diagnostics: the first nested handler runs
        const bool logRun = s_dodLogged < 6u;
        if (logRun)
            ++s_dodLogged;
        std::string trail;
        uint32_t steps = 0;
        for (; context.pc != 0u && steps < 4000000u; ++steps)
        {
            if (logRun && steps < 80u)
            {
                char item[16];
                std::snprintf(item, sizeof(item), " %X", context.pc);
                trail += item;
            }
            if (!m_runtime.hasFunction(context.pc))
            {
                std::cerr << "[ee] interrupt handler 0x" << std::hex << handler.handler << " reached unknown pc 0x"
                          << context.pc << std::dec << std::endl;
                break;
            }
            PS2Runtime::RecompiledFunction function = m_runtime.lookupFunction(context.pc);
            try
            {
                g_ps2GuestUnwinding = false;
                dodVu0Load(context);
                const int savedThreadId = g_dodCurrentThreadId;
                g_dodCurrentThreadId = -1;
                function(m_rdram, &context, &m_runtime);
                g_dodCurrentThreadId = savedThreadId;
                dodVu0Save(context);
            }
            catch (const EeDispatcherTransfer &)
            {
                std::cerr << "[ee] interrupt handler 0x" << std::hex << handler.handler
                          << " tried to switch threads; abandoned at pc 0x" << context.pc << std::dec << std::endl;
                break;
            }
            catch (const std::exception &error)
            {
                std::cerr << "[ee] interrupt handler 0x" << std::hex << handler.handler << " failed at pc 0x" << context.pc
                          << std::dec << ": " << error.what() << std::endl;
                break;
            }
        }
        if (logRun)
        {
            std::cerr << "[ee] sync interrupt cause " << cause << " handler 0x" << std::hex << handler.handler
                      << " steps=" << std::dec << steps << " end pc=0x" << std::hex << context.pc << std::dec
                      << " trail:" << trail << std::endl;
        }
    }
}

void EeScheduler::setVSyncFlag(uint32_t flagAddress, uint32_t tickAddress)
{
    assertExecutor();
    m_vsyncFlagAddress = flagAddress;
    m_vsyncTickAddress = tickAddress;
    writeGuestU32(flagAddress, 0u);
    if (tickAddress != 0u)
    {
        const uint32_t physical = tickAddress & 0x1FFFFFFFu;
        if (m_rdram && physical <= PS2_RAM_SIZE - sizeof(uint64_t))
        {
            const uint64_t zero = 0u;
            std::memcpy(m_rdram + physical, &zero, sizeof(zero));
        }
    }
}

uint64_t EeScheduler::currentVSyncTick() const noexcept
{
    return m_vsyncTick;
}

uint32_t EeScheduler::setGsVSyncCallback(uint32_t callback, uint32_t gp, uint32_t sp)
{
    assertExecutor();
    (void)sp;
    const uint32_t previous = m_gsVSyncCallback;
    m_gsVSyncCallback = callback;
    m_gsVSyncCallbackGp = gp;
    m_gsVSyncCallbackSp = 0u;
    return previous;
}

[[noreturn]] void EeScheduler::waitVSync(uint64_t afterTick, int fixedResult, std::function<void(R5900Context &)> completion)
{
    blockCurrent(EeWaitState{
        EeWaitReason::VSync,
        EeVSyncWait{afterTick, fixedResult},
        std::move(completion)});
}

void EeScheduler::completeVSync(uint64_t tick)
{
    assertExecutor();
    std::vector<int> completed;
    for (const auto &[id, candidate] : m_threads)
    {
        if ((candidate.status == EeThreadStatus::Waiting || candidate.status == EeThreadStatus::WaitingSuspended) &&
            candidate.wait.reason == EeWaitReason::VSync &&
            std::get<EeVSyncWait>(candidate.wait.payload).afterTick < tick)
        {
            completed.push_back(id);
        }
    }
    std::sort(completed.begin(), completed.end());
    for (const int id : completed)
    {
        GuestThread *waiter = thread(id);
        assert(waiter != nullptr);
        const EeVSyncWait wait = std::get<EeVSyncWait>(waiter->wait.payload);
        const int result = wait.fixedResult >= 0
                               ? wait.fixedResult
                               : static_cast<int>((tick - 1u) & 1u);
        makeReady(*waiter, result, false);
    }
    publishSnapshot();
}

void EeScheduler::completeExternalWait(uint32_t type, uint64_t token, int result)
{
    assertExecutor();
    std::vector<int> completed;
    for (const auto &[id, candidate] : m_threads)
    {
        if ((candidate.status != EeThreadStatus::Waiting && candidate.status != EeThreadStatus::WaitingSuspended) ||
            (candidate.wait.reason != EeWaitReason::External &&
             candidate.wait.reason != EeWaitReason::Mpeg))
        {
            continue;
        }
        const auto &external = std::get<EeExternalWait>(candidate.wait.payload);
        if (external.type == type && external.token == token)
        {
            completed.push_back(id);
        }
    }
    std::sort(completed.begin(), completed.end());
    for (const int id : completed)
    {
        GuestThread *waiter = thread(id);
        assert(waiter != nullptr);
        makeReady(*waiter, result, false);
    }
    publishSnapshot();
}

[[noreturn]] void EeScheduler::waitExternal(EeWaitReason reason,
                                            uint32_t type,
                                            uint64_t token,
                                            std::function<void(R5900Context &)> completion)
{
    EeWaitState wait{reason, EeExternalWait{type, token}, std::move(completion)};
    blockCurrent(std::move(wait));
}

GuestThread *EeScheduler::thread(int id)
{
    auto it = m_threads.find(id);
    return it == m_threads.end() ? nullptr : &it->second;
}

const GuestThread *EeScheduler::thread(int id) const
{
    auto it = m_threads.find(id);
    return it == m_threads.end() ? nullptr : &it->second;
}

EeSemaphore *EeScheduler::semaphore(int id)
{
    auto it = m_semaphores.find(id);
    return it == m_semaphores.end() ? nullptr : &it->second;
}

const EeSemaphore *EeScheduler::semaphore(int id) const
{
    auto it = m_semaphores.find(id);
    return it == m_semaphores.end() ? nullptr : &it->second;
}

EeEventFlag *EeScheduler::eventFlag(int id)
{
    auto it = m_eventFlags.find(id);
    return it == m_eventFlags.end() ? nullptr : &it->second;
}

const EeEventFlag *EeScheduler::eventFlag(int id) const
{
    auto it = m_eventFlags.find(id);
    return it == m_eventFlags.end() ? nullptr : &it->second;
}

GuestThread *EeScheduler::currentThread()
{
    return thread(m_currentThreadId);
}

const GuestThread *EeScheduler::currentThread() const
{
    return thread(m_currentThreadId);
}

int EeScheduler::currentThreadId() const noexcept
{
    return m_currentThreadId;
}

R5900Context *EeScheduler::currentContext()
{
    GuestThread *self = currentThread();
    return self ? &self->activeContext() : nullptr;
}

uint8_t *EeScheduler::rdram() const noexcept
{
    return m_rdram;
}

void EeScheduler::bindMainContextForSyscall(R5900Context &ctx, uint8_t *rdram)
{
    if (m_executorThread == std::thread::id{})
    {
        reset(rdram, ctx);
        GuestThread *main = selectReady();
        assert(main != nullptr);
        makeRunning(*main);
        return;
    }
    assertExecutor();
    m_rdram = rdram;
    if (m_currentThreadId == 0)
    {
        GuestThread *main = thread(kMainThreadId);
        assert(main != nullptr);
        assert(main->status == EeThreadStatus::Ready);
        removeReady(*main);
        makeRunning(*main);
    }
}

EeKernelSnapshot EeScheduler::snapshot() const
{
    std::lock_guard lock(m_snapshotMutex);
    return m_snapshot;
}

void EeScheduler::publishSnapshot()
{
    EeKernelSnapshot next{};
    next.sequence = ++m_snapshotSequence;
    next.eeCycle = m_eeCycle;
    next.sliceEndCycle = m_sliceEndCycle;
    next.nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
    next.runningThreadId = m_currentThreadId;
    next.threads.reserve(m_threads.size());
    for (const auto &[id, item] : m_threads)
    {
        if (id < 0)
        {
            continue;
        }
        EeThreadSnapshot snapshot{};
        snapshot.id = id;
        const R5900Context &context = item.activeContext();
        snapshot.pc = context.pc;
        snapshot.ra = getRegU32(&context, 31);
        snapshot.sp = getRegU32(&context, 29);
        snapshot.contextGp = getRegU32(&context, 28);
        snapshot.entry = item.entry;
        snapshot.stack = item.stack;
        snapshot.stackSize = item.stackSize;
        snapshot.gp = item.gp;
        snapshot.initialPriority = item.initialPriority;
        snapshot.currentPriority = item.currentPriority;
        snapshot.status = item.status;
        snapshot.waitReason = item.wait.reason;
        snapshot.waitId = waitObjectId(item.wait);
        snapshot.suspendCount = item.suspendCount;
        snapshot.wakeupCount = item.wakeupCount;
        snapshot.invocationDepth = static_cast<uint32_t>(item.invocations.size());
        next.threads.push_back(snapshot);
    }
    std::sort(next.threads.begin(), next.threads.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    next.semaphores.reserve(m_semaphores.size());
    for (const auto &[id, item] : m_semaphores)
    {
        next.semaphores.push_back(EeSemaphoreSnapshot{id,
                                                      item.count,
                                                      item.maxCount,
                                                      static_cast<uint32_t>(item.waiters.size())});
    }
    std::sort(next.semaphores.begin(), next.semaphores.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    next.eventFlags.reserve(m_eventFlags.size());
    for (const auto &[id, item] : m_eventFlags)
    {
        next.eventFlags.push_back(EeEventFlagSnapshot{id,
                                                      item.bits,
                                                      item.initBits,
                                                      item.attr,
                                                      static_cast<uint32_t>(item.waiters.size())});
    }
    std::sort(next.eventFlags.begin(), next.eventFlags.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    {
        std::lock_guard lock(m_snapshotMutex);
        m_snapshot = std::move(next);
    }
}

void EeScheduler::assertExecutor() const
{
    assert(m_executorThread == std::this_thread::get_id());
}

int EeScheduler::allocateThreadId()
{
    for (int attempts = 0; attempts <= kLastThreadId - kFirstThreadId; ++attempts)
    {
        const int candidate = m_nextThreadId;
        m_nextThreadId = candidate == kLastThreadId ? kFirstThreadId : candidate + 1;
        if (!m_threads.contains(candidate))
        {
            return candidate;
        }
    }
    return 0;
}

GuestThread &EeScheduler::acquireInvocationThread()
{
    for (auto &[id, candidate] : m_threads)
    {
        if (id < 0 && candidate.status == EeThreadStatus::Dormant && candidate.invocations.empty())
        {
            return candidate;
        }
    }

    GuestThread dispatcher{};
    dispatcher.id = m_nextInvocationThreadId--;
    dispatcher.initialPriority = 0;
    dispatcher.currentPriority = 0;
    dispatcher.status = EeThreadStatus::Dormant;
    return m_threads.emplace(dispatcher.id, std::move(dispatcher)).first->second;
}

void EeScheduler::enqueueReady(GuestThread &item, bool front)
{
    if (auto *perf = dodWaitPerf(this)) perf->ready(item.id);
    assert(item.currentPriority >= 0 && item.currentPriority < kPriorityCount);
    item.status = EeThreadStatus::Ready;
    auto &queue = m_readyQueues[item.currentPriority];
    if (front)
    {
        queue.push_front(item.id);
    }
    else
    {
        queue.push_back(item.id);
    }
}

void EeScheduler::removeReady(GuestThread &item)
{
    if (item.status != EeThreadStatus::Ready)
    {
        return;
    }
    auto &queue = m_readyQueues[item.currentPriority];
    auto it = std::find(queue.begin(), queue.end(), item.id);
    assert(it != queue.end());
    queue.erase(it);
}

GuestThread *EeScheduler::selectReady()
{
    for (auto &queue : m_readyQueues)
    {
        if (queue.empty())
        {
            continue;
        }
        const int id = queue.front();
        queue.pop_front();
        GuestThread *selected = thread(id);
        assert(selected != nullptr);
        assert(selected->status == EeThreadStatus::Ready);
        return selected;
    }
    return nullptr;
}

void EeScheduler::makeRunning(GuestThread &item)
{
    assert(m_currentThreadId == 0);
    assert(item.status == EeThreadStatus::Ready);
    if (dodDispatchStatsOn())
        ++g_dodDispatch.switches;
    item.status = EeThreadStatus::Running;
    m_currentThreadId = item.id;
    renewTimeSlice();
}

void EeScheduler::makeDormant(GuestThread &item)
{
    if (auto *perf = dodWaitPerf(this)) perf->abandon(item.id);
    removeReady(item);
    removeFromWaitObject(item);
    item.status = EeThreadStatus::Dormant;
    item.wait = {};
    item.resumeCompletion = {};
    item.suspendCount = 0;
    item.wakeupCount = 0;
    item.invocations.clear();
}

void EeScheduler::removeFromWaitObject(GuestThread &item)
{
    const int id = item.id;
    if (item.wait.reason == EeWaitReason::Semaphore)
    {
        const int objectId = std::get<EeSemaphoreWait>(item.wait.payload).id;
        if (EeSemaphore *object = semaphore(objectId))
        {
            auto it = std::find(object->waiters.begin(), object->waiters.end(), id);
            if (it != object->waiters.end())
            {
                object->waiters.erase(it);
            }
        }
    }
    else if (item.wait.reason == EeWaitReason::EventFlag)
    {
        const int objectId = std::get<EeEventFlagWait>(item.wait.payload).id;
        if (EeEventFlag *object = eventFlag(objectId))
        {
            auto it = std::find(object->waiters.begin(), object->waiters.end(), id);
            if (it != object->waiters.end())
            {
                object->waiters.erase(it);
            }
        }
    }
    item.wait = {};
}

void EeScheduler::blockCurrent(EeWaitState wait)
{
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (auto *perf = dodWaitPerf(this)) perf->begin(*self, wait);
    self->wait = std::move(wait);
    self->status = self->suspendCount == 0 ? EeThreadStatus::Waiting : EeThreadStatus::WaitingSuspended;
    m_currentThreadId = 0;
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

void EeScheduler::makeReady(GuestThread &item, int result, bool interruptSafe)
{
    if (auto *perf = dodWaitPerf(this)) perf->release(item.id, result);
    auto completion = std::move(item.wait.completion);
    item.wait = {};
    setReturnS32(&item.activeContext(), result);
    item.resumeCompletion = std::move(completion);
    if (item.suspendCount != 0)
    {
        item.status = EeThreadStatus::Suspended;
        return;
    }
    enqueueReady(item);
    requestPreemptionIfHigher(item, interruptSafe);
}

void EeScheduler::requestPreemptionIfHigher(const GuestThread &readyThread, bool interruptSafe)
{
    const GuestThread *running = currentThread();
    if (!running || readyThread.currentPriority >= running->currentPriority)
    {
        return;
    }
    m_rescheduleRequested = true;
    if (interruptSafe || m_insideInterrupt)
    {
        if (dodDispatchStatsOn())
            ++g_dodDispatch.setPreempt;
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

void EeScheduler::applyPendingPreemption()
{
    if (!m_rescheduleRequested)
    {
        return;
    }
    if (m_currentThreadId == 0)
    {
        m_rescheduleRequested = false;
        m_timeSliceExpired = false;
        return;
    }
    GuestThread *self = currentThread();
    assert(self != nullptr);
    enqueueReady(*self, !m_timeSliceExpired);
    m_currentThreadId = 0;
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
}

void EeScheduler::processPendingEvents()
{
    assertExecutor();
    processDueDeadlines();
    const uint32_t timerInterrupts = m_pendingEeTimerInterrupts;
    m_pendingEeTimerInterrupts = 0u;
    for (uint32_t timer = 0u; timer < 4u; ++timer)
    {
        if ((timerInterrupts & (1u << timer)) != 0u)
        {
            dispatchIrq(false, 9u + timer);
        }
    }
    if (dodEventPumpAlways() || g_dodQueuedEvents.load(std::memory_order_seq_cst) != 0u)
    {
        std::deque<EeEvent> pending;
        {
            std::lock_guard lock(m_eventMutex);
            pending.swap(m_events);
            g_dodQueuedEvents.store(0u, std::memory_order_seq_cst);
        }
        for (const EeEvent &event : pending)
        {
            processEvent(event);
        }
    }

    {
        // [DoD] Lock-free (patch 0018): a post that lands between the check and the store sets the flag again,
        // because the count is re-read after the store.
        const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
        const bool cycleEventDue = nextEventCycle != 0u && m_eeCycle >= nextEventCycle;
        const bool pendingWork = g_dodQueuedEvents.load(std::memory_order_seq_cst) != 0u || cycleEventDue ||
                                 m_stopRequested.load(std::memory_order_acquire);
        m_checkpointPending.store(pendingWork, std::memory_order_seq_cst);
        if (pendingWork && dodDispatchStatsOn())
            ++g_dodDispatch.setPendingWork;
        if (!pendingWork && g_dodQueuedEvents.load(std::memory_order_seq_cst) != 0u)
            m_checkpointPending.store(true, std::memory_order_seq_cst);
    }
    applyPendingPreemption();
}

void EeScheduler::processDueDeadlines()
{
    if (!dodEventPumpAlways())
    {
        // [DoD] Nothing can be due before the earliest deadline cycle (patch 0018): no lock, no host clock read.
        const uint64_t next = m_nextDeadlineCycle.load(std::memory_order_acquire);
        if (next == 0u || m_eeCycle < next)
            return;
    }
    DodVBlankPerfBatch perfBatch;
    for (;;)
    {
        std::vector<ScheduledEvent> due;
        std::chrono::steady_clock::time_point pacingDeadline{};
        {
            std::unique_lock lock(m_eventMutex);
            const auto now = std::chrono::steady_clock::now();
            for (const ScheduledEvent &item : m_deadlines)
            {
                if (item.deadlineCycle <= m_eeCycle &&
                    (pacingDeadline == std::chrono::steady_clock::time_point{} ||
                     item.hostDeadline < pacingDeadline))
                {
                    pacingDeadline = item.hostDeadline;
                }
            }

            if (pacingDeadline == std::chrono::steady_clock::time_point{})
            {
                updateNextDeadline();
                return;
            }

            if (now < pacingDeadline)
            {
                m_eventCv.wait_until(lock, pacingDeadline, [this]()
                                     { return !m_events.empty() ||
                                              m_stopRequested.load(std::memory_order_acquire); });
                if (!m_events.empty() || m_stopRequested.load(std::memory_order_acquire))
                {
                    updateNextDeadline();
                    return;
                }
            }

            const auto pacedNow = std::chrono::steady_clock::now();
            auto firstFuture = std::partition(m_deadlines.begin(), m_deadlines.end(),
                                              [this, pacedNow](const ScheduledEvent &item)
                                              { return item.deadlineCycle <= m_eeCycle &&
                                                       item.hostDeadline <= pacedNow; });
            due.insert(due.end(),
                       std::make_move_iterator(m_deadlines.begin()),
                       std::make_move_iterator(firstFuture));
            m_deadlines.erase(m_deadlines.begin(), firstFuture);
            updateNextDeadline();
        }

        std::sort(due.begin(), due.end(), [](const ScheduledEvent &left, const ScheduledEvent &right)
                  {
                      if (left.deadlineCycle != right.deadlineCycle)
                      {
                          return left.deadlineCycle < right.deadlineCycle;
                      }
                      if (left.event.type != right.event.type)
                      {
                          return left.event.type < right.event.type;
                      }
                      if (left.event.id != right.event.id)
                      {
                          return left.event.id < right.event.id;
                      }
                      return left.sequence < right.sequence; });

        if (due.empty())
        {
            return;
        }

        for (ScheduledEvent &scheduled : due)
        {
            if (scheduled.event.type == EeEventType::VBlankStart)
            {
                // [DoD] ran-j/PS2Recomp#332: the next field is paced from max(this field's host deadline, now - 100 ms).
                // An absolute schedule never forgives lateness: after a slow phase (loading, a movie) the game ran
                // above real time until the whole debt was repaid, the SPU2 made sound faster than it plays and the
                // host queue had to drop some (a small pop about a minute in). Stalls up to 100 ms are still caught up.
                // Only host waiting changes: the field still comes at the same EE cycle.
                const auto pacedBase = std::max(scheduled.hostDeadline,
                                                std::chrono::steady_clock::now() - kMaxVBlankPacingDebt);
                scheduleEvent(scheduled.deadlineCycle + kVBlankDurationCycles,
                              pacedBase + kVBlankDuration,
                              EeEvent{EeEventType::VBlankEnd, 0, m_vsyncTick + 1u});
                scheduleEvent(scheduled.deadlineCycle + kVBlankPeriodCycles,
                              pacedBase + dodVBlankHostPeriod(),
                              EeEvent{EeEventType::VBlankStart, 0, 0});
                perfBatch.dispatch(scheduled.hostDeadline);
            }
            processEvent(scheduled.event);
        }
    }
}

// [DoD] Host time of one field: NTSC 60/1.001 Hz (kVBlankPeriod) by default; the runner's vsync lock (vsync = lock in
// dod.ini) sets 60.000 Hz.
std::atomic<int64_t> g_dodVBlankPeriodNs{16683350};

void EeScheduler::processEvent(const EeEvent &event)
{
    if (dodDispatchStatsOn())
        ++g_dodDispatch.processed[static_cast<size_t>(event.type) & 7u];
    switch (event.type)
    {
    case EeEventType::Stop:
        requestStop();
        break;
    case EeEventType::VBlankStart:
        if (auto *perf = dodWaitPerf(this)) perf->report();
        ++m_vsyncTick;
        m_runtime.memory().gs().vsyncTick.store(m_vsyncTick, std::memory_order_release);
        if ((m_vsyncTick & 1u) != 0u)
        {
            m_runtime.memory().gs().csr.fetch_or(0x2000ull, std::memory_order_acq_rel);
        }
        else
        {
            m_runtime.memory().gs().csr.fetch_and(~0x2000ull, std::memory_order_acq_rel);
        }
        dodpcsx2::vsync(m_runtime.memory().gs()); // [DoD] PCSX2's GS composes and presents the field
        dodCallHistTick(m_vsyncTick);
        {
            // [DoD] DOD_RAM_DUMP=<dir> (RUNTIME_PATCHES row 124): every 300 fields, EE RAM 0x3FF000-0x932000 (DoD's data
            // and bss) to <dir>\tick_<n>.bin, to find the game's own counters (its frame counter) and read the same
            // word in PCSX2 through PINE for a like-for-like comparison.
            static const char *dumpDir = std::getenv("DOD_RAM_DUMP");
            if (dumpDir && m_rdram && m_vsyncTick % 300u == 0u)
            {
                char path[512];
                std::snprintf(path, sizeof(path), "%s\\tick_%06llu.bin", dumpDir, static_cast<unsigned long long>(m_vsyncTick));
                if (FILE *f = std::fopen(path, "wb"))
                {
                    std::fwrite(m_rdram + 0x3FF000u, 1, 0x932000u - 0x3FF000u, f);
                    std::fclose(f);
                }
                std::fprintf(stderr, "[ram-dump] tick %llu\n", static_cast<unsigned long long>(m_vsyncTick));
            }
        }
        {
            // [DoD] DOD_THREAD_DUMP=<fields>: every n fields, each EE thread's state, priority, wait and pc.
            static const uint64_t every = []() -> uint64_t { const char *v = std::getenv("DOD_THREAD_DUMP"); return v ? std::strtoull(v, nullptr, 10) : 0u; }();
            if (every != 0u && (m_vsyncTick % every) == 0u)
            {
                std::vector<int> ids;
                for (const auto &entry : m_threads)
                    ids.push_back(entry.first);
                std::sort(ids.begin(), ids.end());
                std::fprintf(stderr, "[threads] tick %llu current %d\n", static_cast<unsigned long long>(m_vsyncTick), m_currentThreadId);
                {
                    PS2Memory &mem = m_runtime.memory();
                    std::fprintf(stderr, "[threads]  dmac D_STAT %08X D_PCR %08X | ch3 fromIPU chcr %08X qwc %X madr %08X | ch4 toIPU chcr %08X qwc %X madr %08X tadr %08X | IPU ctrl %08X cmd %08X:%08X bp %08X | intc mask %08X dmac mask %08X\n",
                                 mem.readIORegister(0x1000E010u), mem.readIORegister(0x1000E020u),
                                 mem.readIORegister(0x1000B000u), mem.readIORegister(0x1000B020u), mem.readIORegister(0x1000B010u),
                                 mem.readIORegister(0x1000B400u), mem.readIORegister(0x1000B420u), mem.readIORegister(0x1000B410u),
                                 mem.readIORegister(0x1000B430u), mem.read32(0x10002010u), mem.read32(0x10002004u),
                                 mem.read32(0x10002000u), mem.read32(0x10002020u), m_enabledIntcMask, m_enabledDmacMask);
                    std::fprintf(stderr, "[threads]  latched masked intc %08X dmac %08X\n", g_dodPendingMaskedIntc, g_dodPendingMaskedDmac);
                    std::fprintf(stderr, "[threads]  chcr vif0 %08X vif1 %08X gif %08X sif0 %08X sif1 %08X sif2 %08X sprF %08X sprT %08X | qwc vif1 %X gif %X | GIF_STAT %08X VIF1_STAT %08X\n",
                                 mem.readIORegister(0x10008000u), mem.readIORegister(0x10009000u), mem.readIORegister(0x1000A000u),
                                 mem.readIORegister(0x1000C000u), mem.readIORegister(0x1000C400u), mem.readIORegister(0x1000C800u),
                                 mem.readIORegister(0x1000D000u), mem.readIORegister(0x1000D400u),
                                 mem.readIORegister(0x10009020u), mem.readIORegister(0x1000A020u),
                                 mem.readIORegister(0x10003020u), mem.readIORegister(0x10003C00u));
                }
                for (const int id : ids)
                {
                    const GuestThread &t = m_threads.at(id);
                    int waitObj = 0;
                    if (const auto *s = std::get_if<EeSemaphoreWait>(&t.wait.payload)) waitObj = s->id;
                    else if (const auto *e = std::get_if<EeEventFlagWait>(&t.wait.payload)) waitObj = e->id;
                    const R5900Context &c = t.activeContext();
                    std::fprintf(stderr, "[threads]  id %d entry %08X status %d prio %d susp %d wakeups %u wait %d obj %d pc %08X ra %08X inv %zu\n",
                                 id, t.entry, static_cast<int>(t.status), t.currentPriority, t.suspendCount, t.wakeupCount,
                                 static_cast<int>(t.wait.reason), waitObj, c.pc,
                                 static_cast<uint32_t>(_mm_extract_epi32(c.r[31], 0)), t.invocations.size());
                    for (const GuestInvocation &inv : t.invocations)
                        std::fprintf(stderr, "[threads]    invocation kind %d pc %08X ra %08X\n", static_cast<int>(inv.kind),
                                     inv.context.pc, static_cast<uint32_t>(_mm_extract_epi32(inv.context.r[31], 0)));
                }
                // DOD_THREAD_DUMP_WORDS=<hex addr>,...: guest words printed with each dump.
                static const std::vector<uint32_t> words = []()
                {
                    std::vector<uint32_t> list;
                    if (const char *v = std::getenv("DOD_THREAD_DUMP_WORDS"))
                        for (const char *p = v; *p;)
                        {
                            char *end = nullptr;
                            const unsigned long a = std::strtoul(p, &end, 16);
                            if (end == p)
                                break;
                            list.push_back(static_cast<uint32_t>(a));
                            p = *end ? end + 1 : end;
                        }
                    return list;
                }();
                if (!words.empty() && m_rdram)
                {
                    std::string line = "[threads]  words";
                    char item[32];
                    for (const uint32_t a : words)
                    {
                        uint32_t value = 0u;
                        std::memcpy(&value, m_rdram + (a & 0x1FFFFFCu), sizeof(value));
                        std::snprintf(item, sizeof(item), " %08X=%08X", a, value);
                        line += item;
                    }
                    std::fprintf(stderr, "%s\n", line.c_str());
                }
            }
        }
        writeGuestU32(m_vsyncFlagAddress, 1u);
        if (m_vsyncTickAddress != 0u)
        {
            const uint32_t physical = m_vsyncTickAddress & 0x1FFFFFFFu;
            if (m_rdram && physical <= PS2_RAM_SIZE - sizeof(uint64_t))
            {
                std::memcpy(m_rdram + physical, &m_vsyncTick, sizeof(m_vsyncTick));
            }
        }
        m_vsyncFlagAddress = 0u;
        m_vsyncTickAddress = 0u;
        completeVSync(m_vsyncTick);
        // [DoD] Read-only evidence at an EE execution boundary: follow the
        // original libpad2 DMA buffers through to the game's button edges.
        if (m_rdram)
        {
            static const bool tracePad = std::getenv("DOD_PAD_TRACE") != nullptr;
            if (tracePad)
            {
                const auto word = [&](uint32_t address) {
                    uint32_t value = 0;
                    std::memcpy(&value, m_rdram + address, sizeof(value));
                    return value;
                };
                const uint32_t state = word(0x690348u), current = word(0x690404u), edge = word(0x69040Cu);
                const uint32_t raw = word(0x69042Cu) & 0xFFFFu;
                const uint32_t dma0 = word(0x690D9Cu) & 0xFFFFu, dma1 = word(0x690E1Cu) & 0xFFFFu;
                const uint32_t loop = word(0x817730u), callback = word(0x817734u);
                const uint32_t lastSeq = word(0x6AB758u);
                // Header words preserve all first eight bytes: byte 0 is state,
                // byte 2 is payload length, and the second word is validity.
                const uint32_t hdr00 = word(0x690D80u), hdr01 = word(0x690D84u);
                const uint32_t hdr10 = word(0x690E00u), hdr11 = word(0x690E04u);
                using PadClock = std::chrono::steady_clock;
                struct PadTraceState
                {
                    PadClock::time_point windowStart{};
                    uint64_t previousTick = 0, windowTick = 0;
                    uint32_t windowLoop = 0, windowCallback = 0;
                    uint32_t previousState = ~0u, previousRaw = ~0u, previousCurrent = ~0u,
                             previousEdge = ~0u, previousDma0 = ~0u, previousDma1 = ~0u;
                    unsigned changeLines = 0, rateLines = 0, windowChanges = 0, suppressedChanges = 0;
                };
                static thread_local PadTraceState trace;
                const auto now = PadClock::now();
                if (trace.windowStart == PadClock::time_point{} || m_vsyncTick <= trace.previousTick)
                {
                    trace = PadTraceState{};
                    trace.windowStart = now;
                    trace.windowTick = m_vsyncTick;
                    trace.windowLoop = loop;
                    trace.windowCallback = callback;
                }
                trace.previousTick = m_vsyncTick;
                const double seconds = std::chrono::duration<double>(now - trace.windowStart).count();
                if (seconds >= 1.0)
                {
                    if (trace.rateLines < 600u)
                    {
                        const uint64_t ticks = m_vsyncTick - trace.windowTick;
                        const uint32_t loops = loop - trace.windowLoop;
                        const uint32_t callbacks = callback - trace.windowCallback;
                        std::fprintf(stderr,
                            "[pad:rate] seconds=%.3f tick=%llu vblank_n=%llu vblank_hz=%.2f "
                            "loop=%u loop_n=%u loop_hz=%.2f callback=%u callback_n=%u callback_hz=%.2f "
                            "lastSeq=%u seq0=%u seq1=%u hdr0=%08X:%08X hdr1=%08X:%08X suppressed=%u\n",
                            seconds, static_cast<unsigned long long>(m_vsyncTick),
                            static_cast<unsigned long long>(ticks), ticks / seconds,
                            loop, loops, loops / seconds, callback, callbacks, callbacks / seconds,
                            lastSeq, word(0x690DFCu), word(0x690E7Cu), hdr00, hdr01, hdr10, hdr11,
                            trace.suppressedChanges);
                        ++trace.rateLines;
                    }
                    trace.windowStart = now;
                    trace.windowTick = m_vsyncTick;
                    trace.windowLoop = loop;
                    trace.windowCallback = callback;
                    trace.windowChanges = trace.suppressedChanges = 0;
                }
                const bool changed = state != trace.previousState || current != trace.previousCurrent ||
                    raw != trace.previousRaw || edge != trace.previousEdge ||
                    dma0 != trace.previousDma0 || dma1 != trace.previousDma1;
                // Counters/sequences alone do not emit per-field lines. Bound
                // button/state snapshots to 24 per second and 1024 per run.
                if (changed && trace.changeLines < 1024u && trace.windowChanges < 24u)
                {
                    std::fprintf(stderr, "[pad:ee] tick=%llu state=%u raw=%04X current=%08X edge=%08X dma0=%04X dma1=%04X seq0=%u seq1=%u loop=%u callback=%u lastSeq=%u hdr0=%08X:%08X hdr1=%08X:%08X\n",
                        static_cast<unsigned long long>(m_vsyncTick), state, raw, current, edge, dma0, dma1,
                        word(0x690DFCu), word(0x690E7Cu), loop, callback, lastSeq, hdr00, hdr01, hdr10, hdr11);
                    ++trace.changeLines;
                    ++trace.windowChanges;
                }
                else if (changed)
                    ++trace.suppressedChanges;
                trace.previousState = state; trace.previousRaw = raw; trace.previousCurrent = current;
                trace.previousEdge = edge; trace.previousDma0 = dma0; trace.previousDma1 = dma1;
            }
        }
        if (m_gsVSyncCallback != 0u && m_runtime.hasFunction(m_gsVSyncCallback))
        {
            GuestInvocation invocation{};
            invocation.kind = GuestInvocationKind::GsCallback;
            invocation.context.pc = m_gsVSyncCallback;
            SET_GPR_U32(&invocation.context, 4, static_cast<uint32_t>(m_vsyncTick));
            SET_GPR_U32(&invocation.context, 28, m_gsVSyncCallbackGp);
            SET_GPR_U32(&invocation.context, 29, m_gsVSyncCallbackSp);
            SET_GPR_U32(&invocation.context, 31, 0u);
            queueInvocation(std::move(invocation));
        }
        dispatchIrq(false, 2u);
        break;
    case EeEventType::ExternalWake:
        completeExternalWait(event.id, event.value, KE_OK);
        break;
    case EeEventType::VBlankEnd:
        dispatchIrq(false, 3u);
        break;
    case EeEventType::Dmac:
        // [DoD] Wake-up for work the IOP thread posted (threaded IOP mode): apply it here, on the EE thread.
        if (event.id == kIopPostedWorkEventId)
            ps2IopDrainPosted(m_runtime);
        break;
    case EeEventType::Alarm:
    {
        auto it = m_alarms.find(static_cast<int>(event.id));
        if (it == m_alarms.end())
        {
            break;
        }
        const EeAlarm alarm = it->second;
        m_alarms.erase(it);
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::Alarm;
        invocation.context.pc = alarm.handler;
        SET_GPR_U32(&invocation.context, 4, static_cast<uint32_t>(alarm.id));
        SET_GPR_U32(&invocation.context, 5, static_cast<uint32_t>(alarm.ticks));
        SET_GPR_U32(&invocation.context, 6, alarm.argument);
        SET_GPR_U32(&invocation.context, 28, alarm.gp);
        SET_GPR_U32(&invocation.context, 29, 0u);
        SET_GPR_U32(&invocation.context, 31, 0u);
        queueInvocation(std::move(invocation));
        break;
    }
    }
}

void EeScheduler::finishEventWaiters(EeEventFlag &flag, bool interruptSafe)
{
    for (auto it = flag.waiters.begin(); it != flag.waiters.end();)
    {
        GuestThread *waiter = thread(*it);
        assert(waiter != nullptr);
        const EeEventFlagWait wait = std::get<EeEventFlagWait>(waiter->wait.payload);
        if (!eventCondition(flag.bits, wait.bits, wait.mode))
        {
            ++it;
            continue;
        }
        const uint32_t observed = flag.bits;
        writeGuestU32(wait.resultAddress, observed);
        if ((wait.mode & WEF_CLEAR_ALL) != 0u)
        {
            flag.bits = 0;
        }
        else if ((wait.mode & WEF_CLEAR) != 0u)
        {
            flag.bits &= ~wait.bits;
        }
        it = flag.waiters.erase(it);
        makeReady(*waiter, KE_OK, interruptSafe);
    }
}

bool EeScheduler::eventCondition(uint32_t current, uint32_t requested, uint32_t mode)
{
    return (mode & WEF_OR) != 0u ? (current & requested) != 0u
                                 : (current & requested) == requested;
}

int EeScheduler::waitObjectId(const EeWaitState &wait)
{
    switch (wait.reason)
    {
    case EeWaitReason::Semaphore:
        return std::get<EeSemaphoreWait>(wait.payload).id;
    case EeWaitReason::EventFlag:
        return std::get<EeEventFlagWait>(wait.payload).id;
    default:
        return 0;
    }
}

void EeScheduler::writeGuestU32(uint32_t address, uint32_t value)
{
    if (address == 0u)
    {
        return;
    }
    const uint32_t physical = address & 0x1FFFFFFFu;
    if (!m_rdram || physical > PS2_RAM_SIZE - sizeof(value))
    {
        return;
    }
    std::memcpy(m_rdram + physical, &value, sizeof(value));
}

void EeScheduler::waitForEvent()
{
    std::unique_lock lock(m_eventMutex);
    if (!m_events.empty() || m_stopRequested.load(std::memory_order_acquire))
    {
        return;
    }
    const uint64_t timerCycles = m_runtime.memory().cyclesUntilNextEeTimerInterrupt();
    const bool hasTimerDeadline = timerCycles != std::numeric_limits<uint64_t>::max();
    if (m_deadlines.empty() && !hasTimerDeadline)
    {
        m_eventCv.wait(lock, [this]()
                       { return !m_events.empty() || m_stopRequested.load(std::memory_order_acquire); });
        return;
    }

    uint64_t deadlineCycle = 0u;
    auto hostDeadline = std::chrono::steady_clock::time_point::max();
    if (!m_deadlines.empty())
    {
        const auto next = std::min_element(m_deadlines.begin(), m_deadlines.end(),
                                           [](const ScheduledEvent &left, const ScheduledEvent &right)
                                           {
                                               if (left.deadlineCycle != right.deadlineCycle)
                                               {
                                                   return left.deadlineCycle < right.deadlineCycle;
                                               }
                                               return left.sequence < right.sequence;
                                           });
        deadlineCycle = next->deadlineCycle;
        hostDeadline = next->hostDeadline;
    }
    if (hasTimerDeadline)
    {
        const auto timerHostDeadline = std::chrono::steady_clock::now() + eeCyclesToHostDuration(timerCycles);
        if (timerHostDeadline < hostDeadline)
        {
            deadlineCycle = m_eeCycle + timerCycles;
            hostDeadline = timerHostDeadline;
        }
    }

    const bool signaled = m_eventCv.wait_until(lock, hostDeadline, [this]()
                                               { return !m_events.empty() ||
                                                        m_stopRequested.load(std::memory_order_acquire); });
    if (!signaled)
    {
        const uint64_t elapsed = deadlineCycle > m_eeCycle ? deadlineCycle - m_eeCycle : 0u;
        lock.unlock();
        uint64_t remaining = elapsed;
        while (remaining > 0u)
        {
            const uint32_t step = static_cast<uint32_t>(std::min<uint64_t>(remaining, std::numeric_limits<uint32_t>::max()));
            accountCycles(step);
            remaining -= step;
        }
        if (dodDispatchStatsOn())
            ++g_dodDispatch.setWake;
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

void EeScheduler::scheduleEvent(uint64_t deadlineCycle,
                                std::chrono::steady_clock::time_point hostDeadline,
                                EeEvent event)
{
    {
        std::lock_guard lock(m_eventMutex);
        m_deadlines.push_back(ScheduledEvent{deadlineCycle, hostDeadline, event, ++m_eventSequence});
        updateNextDeadline();
    }
    m_eventCv.notify_one();
}

void EeScheduler::updateNextDeadline()
{
    if (m_deadlines.empty())
    {
        m_nextDeadlineHost = {};
        m_nextDeadlineCycle.store(0u, std::memory_order_release);
        return;
    }
    const auto it = std::min_element(m_deadlines.begin(), m_deadlines.end(),
                                     [](const ScheduledEvent &left, const ScheduledEvent &right)
                                     {
                                         if (left.deadlineCycle != right.deadlineCycle)
                                         {
                                             return left.deadlineCycle < right.deadlineCycle;
                                         }
                                         return left.sequence < right.sequence;
                                     });
    m_nextDeadlineHost = it->hostDeadline;
    m_nextDeadlineCycle.store(it->deadlineCycle, std::memory_order_release);
}

bool EeScheduler::hasReadyAtOrAbovePriority(int priority) const
{
    const int last = std::clamp(priority, 0, kPriorityCount - 1);
    for (int p = 0; p <= last; ++p)
    {
        if (!m_readyQueues[static_cast<size_t>(p)].empty())
        {
            return true;
        }
    }
    return false;
}

void EeScheduler::renewTimeSlice()
{
    m_sliceEndCycle = m_eeCycle + kDefaultTimeSliceCycles;
    m_timeSliceExpired = false;
}

void EeScheduler::copyMainContextToRuntime()
{
    const GuestThread *main = thread(kMainThreadId);
    if (main)
    {
        m_runtime.m_cpuContext = main->context;
    }
}

void EeScheduler::publishDebugContext(const R5900Context &context)
{
    m_runtime.m_debugPc.store(context.pc, std::memory_order_relaxed);
    m_runtime.m_debugRa.store(getRegU32(&context, 31), std::memory_order_relaxed);
    m_runtime.m_debugSp.store(getRegU32(&context, 29), std::memory_order_relaxed);
    m_runtime.m_debugGp.store(getRegU32(&context, 28), std::memory_order_relaxed);
}

void EeScheduler::publishIdleDebugContext()
{
    // Temporary IRQ/RPC/alarm invocations deliberately return to PC=0. Once
    // the scheduler is idle, show a real EE thread context instead of leaving
    // the debugger pinned to that completed dispatcher frame.
    const GuestThread *selected = thread(kMainThreadId);
    if (!selected)
    {
        for (const auto &[id, candidate] : m_threads)
        {
            if (id > 0 && candidate.status != EeThreadStatus::Dormant)
            {
                selected = &candidate;
                break;
            }
        }
    }

    if (selected)
    {
        publishDebugContext(selected->activeContext());
    }
}
