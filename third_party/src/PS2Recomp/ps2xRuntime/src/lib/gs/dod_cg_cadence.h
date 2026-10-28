#pragma once

// Progressive story-CG cadence, on the GS thread. The native player submits
// each decoded frame every two fields, but its deferred uploads can arrive
// either side of a field boundary. Select the existing completed buffer on
// that two-field cadence instead of exposing a 3/1-field pair.
// DOD_CG_CADENCE=keep restores the native display selection.
// DOD_CG_GS_TRACE=<first field>:<count> observes uploads without GPU readback.
namespace dodpcsx2::cgtrace
{
inline bool cadenceOn()
{
    static const bool on = [] {
        const char* v = std::getenv("DOD_CG_CADENCE");
        return !(v && std::strcmp(v, "keep") == 0);
    }();
    return on;
}
struct Path
{
    unsigned left = 0, flag = 0, nreg = 1, index = 0;
    uint64_t regs = 0;
};
struct State
{
    uint64_t first = 0, end = 0, field = 0;
    Path paths[4];
    uint64_t bitblt = 0, trxpos = 0, trxreg = 0;
    unsigned bytes = 0, y = 0, xbytes = 0, slot = 0;
    bool movieRow = false;
    uint64_t rolling[2]{}, hash[2]{}, updates[2]{};
    unsigned nextRow[2]{};
    bool active = false, seenFrame = false, valid[2]{}, heldValid = false;
    uint32_t decoderFrame = 0, stable = 0, phase = 0, newestSlot = 0;
    uint64_t submittedField = 0, newFrameField = 0, completed[2]{}, held = 0;
    uint64_t corrections = 0, unavailable = 0;
};
inline State state = [] {
    State s;
    if (const char* v = std::getenv("DOD_CG_GS_TRACE"))
    {
        char* tail = nullptr;
        s.first = std::strtoull(v, &tail, 10);
        if (tail && *tail == ':')
            s.end = s.first + std::min<uint64_t>(72000, std::strtoull(tail + 1, nullptr, 10)); // up to 20 min: whole movies
    }
    return s;
}();

inline void submitted(uint32_t frame, uint64_t queued, bool eligible)
{
    auto& s = state;
    if (!cadenceOn()) eligible = false;
    if (!eligible || !s.active || (s.seenFrame && frame < s.decoderFrame))
    {
        s.active = eligible;
        s.seenFrame = false;
        s.stable = 0;
        s.valid[0] = s.valid[1] = s.heldValid = false;
        s.nextRow[0] = s.nextRow[1] = 0;
        s.movieRow = false;
    }
    if (!eligible) return;
    s.submittedField = queued;
    if (!s.seenFrame || frame != s.decoderFrame)
    {
        const bool regular = s.seenFrame && frame == s.decoderFrame + 1 &&
                             queued == s.newFrameField + 2;
        s.stable = regular ? std::min(s.stable + 1, 8u) : 0;
        if (!regular) s.heldValid = false;
        s.phase = static_cast<unsigned>((queued + 1) & 1);
        s.newFrameField = queued;
        s.decoderFrame = frame;
        s.seenFrame = true;
    }
}

inline void write(unsigned addr, uint64_t value)
{
    auto& s = state;
    if (addr == 0x50) s.bitblt = value;
    if (addr == 0x51) s.trxpos = value;
    if (addr == 0x52) s.trxreg = value;
    if (addr != 0x53) return;
    const unsigned bp = (s.bitblt >> 32) & 0x3fff;
    const unsigned bw = (s.bitblt >> 48) & 0x3f;
    const unsigned psm = (s.bitblt >> 56) & 0x3f;
    const unsigned w = s.trxreg & 0xfff, h = (s.trxreg >> 32) & 0xfff;
    s.y = (s.trxpos >> 48) & 0x7ff;
    s.xbytes = 0;
    s.bytes = w * h * 4;
    s.slot = bp == 0 ? 0 : 1;
    s.movieRow = (s.active || (s.end && s.field < s.end)) &&
                 (value & 3) == 0 && (bp == 0 || bp == 0x800) && bw == 8 && psm == 0 &&
                 w >= 512 && w <= 640 && h == 1 && s.y >= 24 && s.y < 192 &&
                 ((s.trxpos >> 32) & 0x7ff) == 0;
    if (s.movieRow && s.y == 24)
    {
        s.rolling[s.slot] = 14695981039346656037ull;
        s.nextRow[s.slot] = 24;
    }
    if (s.movieRow && s.nextRow[s.slot] != s.y)
        s.movieRow = false;
}

inline void gif(unsigned path, const uint8_t* data, unsigned bytes)
{
    auto& s = state;
    if (!cadenceOn() && (!s.end || s.field >= s.end)) return;
    auto& p = s.paths[path & 3];
    for (unsigned pos = 0; pos + 16 <= bytes; pos += 16)
    {
        uint64_t lo, hi;
        std::memcpy(&lo, data + pos, 8);
        std::memcpy(&hi, data + pos + 8, 8);
        if (!p.left)
        {
            p.flag = (lo >> 58) & 3;
            p.nreg = (lo >> 60) & 15;
            if (!p.nreg) p.nreg = 16;
            const unsigned n = lo & 0x7fff;
            p.left = p.flag == 0 ? n * p.nreg : p.flag == 1 ? (n * p.nreg + 1) / 2 : n;
            p.index = 0;
            p.regs = hi;
            continue;
        }
        if (p.flag == 0)
        {
            if (((p.regs >> (4 * (p.index % p.nreg))) & 15) == 14)
                write(hi & 0xff, lo);
            ++p.index;
        }
        else if (p.flag == 2)
        {
            if (s.movieRow)
            {
                // Two RGB samples per 32 pixels; ignore padding past width 512.
                if (s.xbytes < 2048 && (s.xbytes & 127) == 0)
                {
                    s.rolling[s.slot] = (s.rolling[s.slot] ^ (lo & 0xffffff)) * 1099511628211ull;
                    s.rolling[s.slot] = (s.rolling[s.slot] ^ (hi & 0xffffff)) * 1099511628211ull;
                }
                // The movie overrides TRXREG.RRW with 640 but sends only
                // the 512 source pixels before starting the next row.
                if (s.xbytes + 16 >= 2048)
                {
                    s.nextRow[s.slot] = s.y + 1;
                    if (s.y == 191)
                    {
                        s.hash[s.slot] = s.rolling[s.slot];
                        ++s.updates[s.slot];
                        s.completed[s.slot] = s.field + 1;
                        s.newestSlot = s.slot;
                        s.valid[s.slot] = s.active;
                        if (s.end && s.field + 1 >= s.first && s.field + 1 < s.end)
                            std::fprintf(stderr, "[cg-upload] field %llu bp %X hash %016llX update %llu\n",
                                         s.field + 1, s.slot ? 0x800 : 0, s.hash[s.slot], s.updates[s.slot]);
                    }
                    s.movieRow = false;
                }
            }
            s.xbytes += 16;
        }
        --p.left;
        // Bulk skip image/REGLIST payloads when no sampled row is in progress.
        // Keep tag continuation across calls even outside a story CG.
        if (p.flag != 0 && !s.movieRow)
        {
            const unsigned skip = std::min(p.left, (bytes - pos - 16) / 16);
            pos += skip * 16;
            p.left -= skip;
            if (p.flag == 2) s.xbytes += skip * 16;
        }
    }
}

inline void select(uint64_t field, Pcsx2GsPrivRegs& priv)
{
    auto& s = state;
    const uint64_t fb = priv.regset1[9];
    if (!s.active || s.stable < 8 || field > s.submittedField + 4 ||
        (priv.regset1[0] & 3) != 2 || (fb != 0x1000 && fb != 0x1040) ||
        !s.valid[s.newestSlot] || field > s.completed[s.newestSlot] + 2)
    {
        s.heldValid = false;
        return;
    }
    if (!s.heldValid || (field & 1) == s.phase)
    {
        s.held = s.hash[s.newestSlot];
        s.heldValid = true;
    }
    const unsigned original = fb == 0x1040 ? 1 : 0;
    unsigned selected = original;
    if (!s.valid[selected] || s.hash[selected] != s.held)
    {
        selected ^= 1;
        if (!s.valid[selected] || s.hash[selected] != s.held)
        {
            ++s.unavailable;
            s.heldValid = false;
            return; // no invented frame or replay of an overwritten buffer
        }
    }
    if (selected != original)
    {
        priv.regset1[9] = (fb & ~0x1ffull) | (selected ? 0x40 : 0);
        ++s.corrections;
    }
}

inline void vsync(uint64_t field, const Pcsx2GsPrivRegs& priv, double presentMs)
{
    auto& s = state;
    s.field = field;
    if (!s.end || field < s.first || field >= s.end) return;
    const auto now = std::chrono::steady_clock::now();
    static const auto start = now;
    const double ms = std::chrono::duration<double, std::milli>(now - start).count();
    std::fprintf(stderr, "[cg-gs] field %llu fb1 %llX fb2 %llX pmode %llX h0 %016llX h1 %016llX u0 %llu u1 %llu ms %.3f present %.3f active %u stable %u phase %u corrections %llu unavailable %llu\n",
                 field, priv.regset1[7], priv.regset1[9], priv.regset1[0],
                 s.hash[0], s.hash[1], s.updates[0], s.updates[1], ms, presentMs,
                 unsigned(s.active), s.stable, s.phase, s.corrections, s.unavailable);
}
}
