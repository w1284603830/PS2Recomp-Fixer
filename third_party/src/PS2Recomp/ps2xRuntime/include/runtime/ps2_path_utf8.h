#pragma once
#include <cstddef>
#include <string_view>

namespace ps2::paths
{
// Validate before interpreting ASCII path separators. In a legacy DBCS encoding,
// 0x5c can be a trail byte; treating those bytes as UTF-8 can change the path.
inline bool validUtf8(std::string_view value)
{
    std::size_t i = 0;
    while (i < value.size())
    {
        const auto lead = static_cast<unsigned char>(value[i++]);
        if (lead < 0x80) { if (lead == 0) return false; continue; }
        std::size_t remaining;
        unsigned char low = 0x80, high = 0xbf;
        if (lead >= 0xc2 && lead <= 0xdf) remaining = 1;
        else if (lead >= 0xe0 && lead <= 0xef)
        {
            remaining = 2;
            if (lead == 0xe0) low = 0xa0; // No overlong encoding.
            if (lead == 0xed) high = 0x9f; // No UTF-16 surrogate.
        }
        else if (lead >= 0xf0 && lead <= 0xf4)
        {
            remaining = 3;
            if (lead == 0xf0) low = 0x90;
            if (lead == 0xf4) high = 0x8f; // At most U+10FFFF.
        }
        else return false;
        if (remaining > value.size() - i) return false;
        const auto first = static_cast<unsigned char>(value[i++]);
        if (first < low || first > high) return false;
        while (--remaining)
        {
            const auto next = static_cast<unsigned char>(value[i++]);
            if (next < 0x80 || next > 0xbf) return false;
        }
    }
    return true;
}
}
