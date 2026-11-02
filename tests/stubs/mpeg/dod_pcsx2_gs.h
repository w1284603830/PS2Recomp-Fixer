#pragma once
#include "Common.h"
namespace dodpcsx2
{
    inline bool hdCgSupported() { return false; }
    inline void hdCgFrame(const uint8_t *, uint32_t) {}
}
