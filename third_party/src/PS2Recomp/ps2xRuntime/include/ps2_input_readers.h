#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace ps2input
{
using Reader = uint64_t;

// Protected by the host-pad mutex in production. Each registered native adapter
// acknowledges its own short presses; ordinary SIO2/HLE polling is independent.
class PressReaders
{
    struct Slot { Reader token = 0; uint16_t pending = 0; };
    std::array<Slot, 8> slots{};
    Reader next = 1;
public:
    Reader add()
    {
        for (auto& s : slots) if (!s.token)
        {
            if (!next) ++next;
            s = {next++, 0};
            return s.token;
        }
        return 0;
    }
    bool remove(Reader token)
    {
        for (auto& s : slots) if (token && s.token == token) { s = {}; return true; }
        return false;
    }
    void publish(uint16_t edges) { for (auto& s : slots) if (s.token) s.pending |= edges; }
    void clearPending() { for (auto& s : slots) s.pending = 0; }
    void reset() { slots = {}; } // retain generation: old tokens must not become valid again
    bool consume(Reader token, uint16_t& edges)
    {
        for (auto& s : slots) if (token && s.token == token)
        { edges = s.pending; s.pending = 0; return true; }
        return false;
    }
};

Reader registerReader();
bool unregisterReader(Reader token);
// 32-byte normalized host snapshot. Refresh only input fields in a separately
// validated native packet; this API does not implement a game's libpad ABI.
bool readLatest(Reader token, uint8_t* data, size_t size);
void resetSession(); // before execution / after load; invalidates registered readers
}
