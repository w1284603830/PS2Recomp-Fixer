#pragma once

#include <cstdint>
#include <charconv>
#include <string_view>

namespace ps2_stubs
{
    inline bool parseProfileAddress(std::string_view text, uint32_t &address)
    {
        const bool hex = text.size() > 2u && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
        if (text.empty())
            return false;
        const char *first = text.data() + (hex ? 2u : 0u);
        const char *last = text.data() + text.size();
        const auto result = std::from_chars(first, last, address, hex ? 16 : 10);
        return result.ec == std::errc{} && result.ptr == last;
    }

    inline bool validProfileRamRange(uint32_t address, uint64_t size, uint32_t alignment)
    {
        const uint32_t segment = address & 0xE0000000u;
        const uint32_t physical = address & 0x1FFFFFFFu;
        return (segment == 0u || segment == 0x20000000u || segment == 0x80000000u || segment == 0xA0000000u) &&
               (address & (alignment - 1u)) == 0u && size <= 32u * 1024u * 1024u &&
               static_cast<uint64_t>(physical) + size <= 32u * 1024u * 1024u;
    }

}
