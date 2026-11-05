#pragma once

#include <charconv>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace GSHostTuning
{
struct PortOptions
{
    bool feedbackCopyGuard = false;
    bool clampFootprint = false;
    bool hdDisableNativeScaling = false;
    uint32_t hdMinimumScale = 1;
    std::string staticPageFormat, staticPageDirectory;
};
inline PortOptions portOptions;
inline void ResetPortOptions() { portOptions = {}; }
inline bool ConfigurePortOption(std::string_view key, std::string_view text)
{
    if (key == "static_page_format") { portOptions.staticPageFormat = text; return true; }
    if (key == "static_page_dir") { portOptions.staticPageDirectory = text; return true; }
    bool* flag = key == "feedback_copy_hpo_guard" ? &portOptions.feedbackCopyGuard :
        key == "clamp_footprint" ? &portOptions.clampFootprint :
        key == "hd_frame_disable_native_scaling" ? &portOptions.hdDisableNativeScaling : nullptr;
    if (flag) { *flag = text == "1"; return true; }
    if (key != "hd_frame_min_scale") return false;
    uint32_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    portOptions.hdMinimumScale = parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
        value >= 1 && value <= 8 ? value : 1;
    return true;
}
inline int FeedbackCopyMargin(float scale, bool nativeHpo)
{
    return portOptions.feedbackCopyGuard && nativeHpo && std::isfinite(scale) && scale > 0 && scale <= 256 ?
        std::max(1, static_cast<int>(std::ceil(scale * 0.5f))) : 1;
}
inline bool PreserveClampFootprint(bool hardware, bool sprite, bool clamped)
{
    return portOptions.clampFootprint && hardware && sprite && clamped;
}

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
