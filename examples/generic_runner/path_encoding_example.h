#pragma once
// Optional port-boundary example. Not installed in either reference runner.
// Identify the encoding independently, then decode the WHOLE bounded guest path
// before parsePs2Path/VFS/libmc normalization. Never try UTF-8 then fall back.
#include "runtime/ps2_path_utf8.h"
#include <limits>
#include <string>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#else
#include <iconv.h>
#endif

namespace port_paths
{
// Explicit Windows mapping variants, not claims about any game's encoding.
enum class Encoding : unsigned { Utf8 = 65001, Windows932 = 932, Windows936 = 936, Windows950 = 950 };

// Failure leaves output untouched. No locale/ACP lookup, substitution or guessing.
inline bool decode(std::string_view raw, Encoding encoding, std::string& output)
{
    if (raw.empty() || raw.find('\0') != std::string_view::npos) return false;
    if (encoding == Encoding::Utf8)
    {
        if (!ps2::paths::validUtf8(raw)) return false;
        std::string converted(raw);
        output = std::move(converted);
        return true;
    }
    const char* codec;
    switch (encoding)
    {
    case Encoding::Windows932: codec = "CP932"; break;
    case Encoding::Windows936: codec = "CP936"; break;
    case Encoding::Windows950: codec = "CP950"; break;
    default: return false;
    }
    std::string converted;
#ifdef _WIN32
    (void)codec;
    if (raw.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return false;
    const auto codepage = static_cast<unsigned>(encoding);
    const int size = static_cast<int>(raw.size());
    const int count = MultiByteToWideChar(codepage, MB_ERR_INVALID_CHARS, raw.data(), size, nullptr, 0);
    if (!count) return false;
    std::wstring wide(static_cast<size_t>(count), L'\0');
    if (MultiByteToWideChar(codepage, MB_ERR_INVALID_CHARS, raw.data(), size, wide.data(), count) != count) return false;
    // Require a lossless round-trip; reject default/best-fit aliases.
    BOOL substituted = FALSE;
    const int backSize = WideCharToMultiByte(codepage, WC_NO_BEST_FIT_CHARS, wide.data(), count, nullptr, 0, nullptr, &substituted);
    if (!backSize || substituted) return false;
    std::string back(static_cast<size_t>(backSize), '\0');
    if (WideCharToMultiByte(codepage, WC_NO_BEST_FIT_CHARS, wide.data(), count, back.data(), backSize, nullptr, &substituted) != backSize || substituted || back != raw) return false;
    const int utf8Size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), count, nullptr, 0, nullptr, nullptr);
    if (!utf8Size) return false;
    converted.resize(static_cast<size_t>(utf8Size));
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), count, converted.data(), utf8Size, nullptr, nullptr) != utf8Size) return false;
#else
    if (raw.size() > (converted.max_size() - 1) / 4) return false;
    const iconv_t converter = iconv_open("UTF-8", codec);
    if (converter == reinterpret_cast<iconv_t>(-1)) return false;
    converted.resize(raw.size() * 4 + 1);
    char* source = const_cast<char*>(raw.data());
    size_t sourceLeft = raw.size(), destinationLeft = converted.size();
    char* destination = converted.data();
    const size_t result = iconv(converter, &source, &sourceLeft, &destination, &destinationLeft);
    iconv_close(converter);
    if (result == static_cast<size_t>(-1) || result != 0 || sourceLeft) return false;
    converted.resize(converted.size() - destinationLeft);
    // The Windows code-page mappings can differ at extension/alias points.
    // Require a round-trip here too; a port must still qualify its mapping.
    const iconv_t reverse = iconv_open(codec, "UTF-8");
    if (reverse == reinterpret_cast<iconv_t>(-1)) return false;
    std::string back(raw.size() + 1, '\0');
    source = converted.data(); sourceLeft = converted.size();
    destination = back.data(); destinationLeft = back.size();
    const size_t reversed = iconv(reverse, &source, &sourceLeft, &destination, &destinationLeft);
    iconv_close(reverse);
    if (reversed == static_cast<size_t>(-1) || reversed != 0 || sourceLeft) return false;
    back.resize(back.size() - destinationLeft);
    if (back != raw) return false;
#endif
    if (!ps2::paths::validUtf8(converted)) return false;
    output = std::move(converted);
    return true;
}
}
