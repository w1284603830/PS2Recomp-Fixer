#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// Optional caller-supplied frame metadata. This observer never selects a
// framebuffer, changes privileged registers, or imposes a two-field cadence.
namespace dodpcsx2::cgtrace
{
struct State
{
    uint64_t first = 0, end = 0, queued = 0, gifBytes = 0;
    uint32_t frame = 0;
    bool active = false;
};
inline State state = [] {
    State s;
    if (const char* v = std::getenv("DOD_CG_GS_TRACE"))
    {
        char* tail = nullptr;
        s.first = std::strtoull(v, &tail, 10);
        if (tail && *tail == ':')
        {
            const uint64_t count = std::min<uint64_t>(72000, std::strtoull(tail + 1, nullptr, 10));
            s.end = s.first > UINT64_MAX - count ? UINT64_MAX : s.first + count;
        }
    }
    return s;
}();
inline bool traceOn() { return state.end > state.first; }
inline void reset()
{
    state.queued = state.gifBytes = 0;
    state.frame = 0;
    state.active = false;
}
inline void submitted(uint32_t frame, uint64_t queued, bool active)
{
    state.frame = frame;
    state.queued = queued;
    state.active = active;
}
inline void gif(unsigned path, const uint8_t* data, unsigned bytes)
{
    if (traceOn() && path >= 1 && path <= 3 && data) state.gifBytes += bytes;
}
template<class Registers>
void vsync(uint64_t field, const Registers& priv, double presentMs)
{
    if (!traceOn() || field < state.first || field >= state.end) return;
    std::fprintf(stderr, "[gs-frame] field=%llu caller_frame=%u active=%d queued=%llu gif_bytes=%llu present_ms=%.3f dispfb=%llx/%llx\n",
        static_cast<unsigned long long>(field), state.frame, state.active,
        static_cast<unsigned long long>(state.queued), static_cast<unsigned long long>(state.gifBytes), presentMs,
        static_cast<unsigned long long>(priv.regset1[7]), static_cast<unsigned long long>(priv.regset1[9]));
    state.gifBytes = 0;
}
}
