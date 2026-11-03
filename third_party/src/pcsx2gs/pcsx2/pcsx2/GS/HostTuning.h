#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace GSHostTuning
{
// IDXGIDevice1::SetMaximumFrameLatency accepts 1..16; 0 resets the driver default.
// An absent/invalid option leaves device configuration untouched.
inline std::optional<uint32_t> FrameLatency(const char* text)
{
    if (!text || !*text || std::string_view(text) == "default")
        return std::nullopt;
    const std::string_view value(text);
    uint32_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result > 16u)
        return std::nullopt;
    return result;
}

struct UiPreloadList
{
    std::vector<std::string> names;
    size_t rejected = 0;
    bool truncated = false;
};

// Pack-owned manifest: replacement basenames without extensions. This performs
// lexical filtering only; the renderer still validates each full TextureName
// against the discovered pack and admits only pc_ui_hd entries.
inline UiPreloadList ParseUiPreloadList(std::string_view text)
{
    UiPreloadList result;
    if (text.size() > 1024u * 1024u)
    {
        result.truncated = true;
        return result;
    }
    if (text.substr(0, 3) == "\xef\xbb\xbf")
        text.remove_prefix(3);
    std::unordered_set<std::string> seen;
    while (!text.empty())
    {
        const size_t eol = text.find('\n');
        std::string_view token = text.substr(0, eol);
        text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);
        const size_t first = token.find_first_not_of(" \t\r");
        if (first == std::string_view::npos)
            continue;
        token = token.substr(first);
        token = token.substr(0, token.find_last_not_of(" \t\r") + 1);
        if (token.front() == '#')
            continue;
        if (token.size() > 128u || token.find_first_not_of("0123456789abcdefABCDEF-rx") != std::string_view::npos)
        {
            ++result.rejected;
            continue;
        }
        if (!seen.emplace(token).second)
            continue;
        if (result.names.size() == 1024u)
        {
            result.truncated = true;
            break;
        }
        result.names.emplace_back(token);
    }
    return result;
}
}
