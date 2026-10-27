#pragma once
#include <algorithm>
#include <chrono>
#include <thread>

// Optional host-window pacing only. EE VBlank, SPU2 and MPEG retain their
// existing clocks. Configure before PS2Runtime::run starts its threads.
inline bool g_dodLimitHost60 = true;

namespace dodhost
{
using Clock = std::chrono::steady_clock;
constexpr auto Period = std::chrono::nanoseconds(16666667);

// PCSX2 VMManager::Internal::Throttle reference (2026-10-27): advance an
// absolute deadline, sleep before the final wait, and discard whole-frame
// lateness after stalls. This implementation is for the host pump, not EE.
// https://github.com/PCSX2/pcsx2/blob/master/pcsx2/VMManager.cpp
struct Schedule
{
    Clock::time_point next;
    explicit Schedule(Clock::time_point now) : next(now + Period) {}
    Clock::time_point advance(Clock::time_point now)
    {
        const auto target = next;
        if (now >= target + Period)
        {
            next += Period * ((now - target) / Period + 1);
            return now; // no accumulated catch-up burst after loading/suspend
        }
        next += Period;
        return target;
    }
};

class Pacer
{
    Schedule schedule{Clock::now()};
    const bool enabled = g_dodLimitHost60;
#ifdef _WIN32
    void* timer = nullptr;
    bool highResolution = false;
#endif
public:
    Pacer();
    ~Pacer();
    Pacer(const Pacer&) = delete;
    Pacer& operator=(const Pacer&) = delete;
    void wait();
};
}
