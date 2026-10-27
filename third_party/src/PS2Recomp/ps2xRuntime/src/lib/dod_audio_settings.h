#pragma once
#include <cerrno>
#include <climits>
#include <cctype>
#include <cstdlib>
#include <string>

namespace dod_audio
{
    // Same integer-percent contract as the launcher: absent/malformed = 100,
    // valid integers clamp to 0..100. This private header does not reach generated code.
    inline int percent(const std::string &value)
    {
        char *end = nullptr;
        errno = 0;
        const long parsed = std::strtol(value.c_str(), &end, 10);
        if (end == value.c_str() || errno == ERANGE || parsed < INT_MIN || parsed > INT_MAX)
            return 100;
        while (*end && std::isspace(static_cast<unsigned char>(*end))) ++end;
        if (*end) return 100;
        return parsed < 0 ? 0 : parsed > 100 ? 100 : static_cast<int>(parsed);
    }
}
