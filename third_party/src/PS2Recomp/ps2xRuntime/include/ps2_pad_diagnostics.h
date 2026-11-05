#pragma once
#include <charconv>
#include <cstdint>
#include <string_view>

namespace ps2input
{
// Explicit diagnostic RAM word, not a game's global or a mapped hardware register.
inline uint32_t diagnosticWordAddress(const char* text)
{
    if (!text || !*text) return 0;
    std::string_view s(text); int radix = 10;
    if (s.starts_with("0x") || s.starts_with("0X")) { s.remove_prefix(2); radix = 16; }
    uint32_t value = 0;
    const auto parsed = std::from_chars(s.data(), s.data()+s.size(), value, radix);
    return !s.empty() && parsed.ec == std::errc{} && parsed.ptr == s.data()+s.size() &&
        value && value <= 0x02000000u-sizeof(uint32_t) ? value : 0;
}
}
