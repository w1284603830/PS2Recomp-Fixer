#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>

// Host scheduling only: none of these helpers advances an EE/IOP/SPU clock.
namespace ps2host
{
using Clock = std::chrono::steady_clock;

inline uint32_t boundedUnsigned(const char* text, uint32_t maximum)
{
    if (!text || !*text)
        return 0;
    uint32_t value = 0;
    for (const char* p = text; *p; ++p)
    {
        if (*p < '0' || *p > '9')
            return 0;
        const uint32_t digit = static_cast<uint32_t>(*p - '0');
        if (digit > maximum || value > (maximum - digit) / 10u)
            return 0;
        value = value * 10u + digit;
    }
    return value;
}

class GsWaitTime
{
    std::atomic<uint64_t> nanoseconds{0};

public:
    // Producer thread only. An already-satisfied predicate contributes no time.
    template <class ConditionVariable, class Lock, class Predicate>
    void wait(ConditionVariable& cv, Lock& lock, Predicate pred)
    {
        if (pred())
            return;
        const auto start = Clock::now();
        cv.wait(lock, pred);
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
        if (elapsed > 0)
            nanoseconds.fetch_add(static_cast<uint64_t>(elapsed), std::memory_order_relaxed);
    }

    uint64_t take() { return nanoseconds.exchange(0, std::memory_order_relaxed); }
};

// Preserve the guest deadline cycles/order. Saturate at the clock's maximum
// rather than overflowing if a diagnostic supplies an extreme duration.
template <class Deadlines>
void shiftHostDeadlines(Deadlines& deadlines, uint64_t waitedNs, uint32_t drainUs)
{
    const uint64_t maximum = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    const uint64_t drainNs = static_cast<uint64_t>(drainUs) * 1000u;
    const auto offset = std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(
        static_cast<int64_t>(std::min(waitedNs, maximum - drainNs) + drainNs)));
    for (auto& item : deadlines)
    {
        const auto latest = Clock::time_point::max() - offset;
        item.hostDeadline = item.hostDeadline > latest ? Clock::time_point::max() : item.hostDeadline + offset;
    }
}
}
