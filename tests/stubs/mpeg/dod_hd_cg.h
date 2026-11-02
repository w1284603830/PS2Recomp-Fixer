#pragma once
#include "Common.h"
namespace dodhdcg
{
    struct Frame { std::vector<uint8_t> wire; };
    struct Info { int id = 0, count = 0, w = 0, h = 0; };
    class Decoder
    {
    public:
        Info info;
        bool open(int, uint16_t) { return false; }
        uint32_t pictures() const { return 0; }
        template<class Frames> bool feed(const uint8_t *, size_t, Frames &) { return false; }
        template<class Frames> bool flush(Frames &) { return false; }
    };
}
