#pragma once
// Optional bound on how far the IOP may run ahead of the EE while the EE thread is blocked (the EE stall watchdog in
// ps2_runtime.cpp, DOD_EE_STALL_WATCHDOG=capped). By default the watchdog lets the IOP follow a real-time line so a
// blocked EE (a disc read, shader compilation) does not starve the sound queue. That keeps audio continuous, but the IOP
// then runs further than the EE for as long as the stall lasts, and a game that ties speech or music to what the EE
// shows can drift (speech ahead of the event animation after a long load). The capped policy bounds the lead to one
// field: a longer EE pause pauses the media clock too, and the audio queue may then run dry. Which trade-off is right is
// a per-game finding, so nothing selects the capped policy by default.
#include <algorithm>
#include <cstdint>

namespace ps2host
{
    // One NTSC field of EE cycles (294.912 MHz * 1001 / 60000).
    inline constexpr uint64_t kMaxIopLeadCycles = 294912000ull * 1001ull / 60000ull;

    // The EE cycle count the IOP may run to: the host line when it is not ahead of the EE by more than `maxLead`,
    // otherwise `ee + maxLead`. Never below `ee`. Written as a difference so it cannot wrap near UINT64_MAX.
    inline constexpr uint64_t iopBudget(uint64_t ee, uint64_t hostLine, uint64_t maxLead = kMaxIopLeadCycles) noexcept
    {
        return hostLine <= ee ? ee : ee + std::min(hostLine - ee, maxLead);
    }
}
