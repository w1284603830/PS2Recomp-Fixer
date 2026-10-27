#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// [DoD] Opt-in timings; no scheduling, rendering or guest-state changes.
namespace dodperf
{
enum Stage { HostFrame, Upload, Latch, StateWait, BackendWait, VramWait,
             VramCopy, Convert, RecordWait, Draw, Transfer, EndDrawing, Export, GsInterrupt, IpuRun, Count };
inline constexpr const char *names[Count] = {"host_frame", "upload", "gs_latch", "state_wait",
    "backend_wait", "vram_wait", "vram_copy", "scanout", "record_wait", "raster",
    "transfer", "end_drawing", "frame_export", "gs_interrupt", "ipu_run"};
struct Counter { std::atomic<uint64_t> calls{0}, ns{0}, maxNs{0}; };
inline std::array<Counter, Count> counters;
inline std::array<std::atomic<uint64_t>, 1001> hostFrameHistogram{};
inline uint64_t tickTotal = 0, tickDistinct = 0, tickRepeats = 0, tickJumps = 0, tickMaxJump = 0;
inline bool enabled() { static const bool value = std::getenv("DOD_PERF") != nullptr; return value; }
inline uint64_t now()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
inline uint64_t begin() { return enabled() ? now() : 0; }
inline void end(Stage stage, uint64_t start)
{
    if (!start) return;
    const uint64_t ns = now() - start;
    if (stage == HostFrame)
        hostFrameHistogram[static_cast<size_t>(ns / 100000u < 1000u ? ns / 100000u : 1000u)]
            .fetch_add(1, std::memory_order_relaxed);
    auto &c = counters[stage];
    c.calls.fetch_add(1, std::memory_order_relaxed);
    c.ns.fetch_add(ns, std::memory_order_relaxed);
    uint64_t old = c.maxNs.load(std::memory_order_relaxed);
    while (old < ns && !c.maxNs.compare_exchange_weak(old, ns, std::memory_order_relaxed)) {}
}
struct Scope
{
    Stage stage; uint64_t start;
    explicit Scope(Stage s) : stage(s), start(begin()) {}
    ~Scope() { end(stage, start); }
};
// Main-thread sampling: delta preserves VBlanks that occur between host frames.
inline void observeTick(uint64_t tick)
{
    if (!enabled()) return;
    static bool initialized = false;
    static uint64_t last = 0;
    if (initialized && tick >= last)
    {
        const uint64_t delta = tick - last;
        tickTotal += delta;
        if (delta) ++tickDistinct; else ++tickRepeats;
        if (delta > 1u) ++tickJumps;
        if (delta > tickMaxJump) tickMaxJump = delta;
    }
    initialized = true;
    last = tick;
}
inline void report()
{
    if (!enabled()) return;
    static uint64_t previous = now();
    const uint64_t current = now();
    if (current - previous < 5000000000ull) return;
    const double seconds = (current - previous) / 1e9;
    previous = current;
    std::fprintf(stderr, "[perf] interval=%.3fs", seconds);
    std::fprintf(stderr, " guest_vblank_hz=%.2f tick_distinct=%llu tick_repeat=%llu tick_jumps=%llu tick_max_jump=%llu",
                 tickTotal / seconds, static_cast<unsigned long long>(tickDistinct),
                 static_cast<unsigned long long>(tickRepeats), static_cast<unsigned long long>(tickJumps),
                 static_cast<unsigned long long>(tickMaxJump));
    tickTotal = tickDistinct = tickRepeats = tickJumps = tickMaxJump = 0;
    std::array<uint64_t, 1001> histogram{};
    uint64_t frameCount = 0;
    for (size_t i = 0; i < histogram.size(); ++i)
        frameCount += histogram[i] = hostFrameHistogram[i].exchange(0, std::memory_order_relaxed);
    auto percentile = [&](uint64_t percent) {
        const uint64_t target = (frameCount * percent + 99u) / 100u;
        uint64_t cumulative = 0;
        for (size_t i = 0; i < histogram.size(); ++i)
            if ((cumulative += histogram[i]) >= target) return (i + 1) / 10.0;
        return 100.1;
    };
    if (frameCount) std::fprintf(stderr, " host_p95=%.1fms host_p99=%.1fms", percentile(95), percentile(99));
    for (int i = 0; i < Count; ++i)
    {
        auto &c = counters[i];
        const uint64_t calls = c.calls.exchange(0, std::memory_order_relaxed);
        const uint64_t ns = c.ns.exchange(0, std::memory_order_relaxed);
        const uint64_t maxNs = c.maxNs.exchange(0, std::memory_order_relaxed);
        if (!calls) continue;
        if (i == HostFrame) std::fprintf(stderr, " fps=%.2f", calls / seconds);
        std::fprintf(stderr, " %s[n=%llu avg=%.3fms max=%.3fms total=%.1fms]",
                     names[i], static_cast<unsigned long long>(calls),
                     ns / 1e6 / calls, maxNs / 1e6, ns / 1e6);
    }
    std::fputc('\n', stderr);
}
}
