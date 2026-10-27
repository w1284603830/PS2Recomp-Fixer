// [DoD] PS2 IPU model. The decoding paths (IDEC, FDEC, SETIQ, SETVQ, SETTH, CSC) follow PCSX2's
// pcsx2/IPU/IPU.cpp, IPU_MultiISA.cpp, yuv2rgb.cpp and IPUdither.cpp (GPL-2.0+ / GPL-3.0+), which in turn are
// based on libmpeg2 (Michel Lespinasse, Aaron Holtzman). See ps2_ipu.h for the model.

#include "ps2_ipu.h"
#include "ps2_ipu_vlc.h"
#include "gs/dod_perf.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace ps2_ipu
{
    namespace
    {
        constexpr uint32_t kCtrlEcd = 1u << 14;
        constexpr uint32_t kCtrlScd = 1u << 15;
        constexpr uint32_t kCtrlRst = 1u << 30;

        enum Command : uint32_t
        {
            BCLR = 0,
            IDEC = 1,
            BDEC = 2,
            VDEC = 3,
            FDEC = 4,
            SETIQ = 5,
            SETVQ = 6,
            CSC = 7,
            PACK = 8,
            SETTH = 9,
        };

        constexpr int kNonLinearQuantizerScale[32] = {
            0, 1, 2, 3, 4, 5, 6, 7,
            8, 10, 12, 14, 16, 18, 20, 22,
            24, 28, 32, 36, 40, 44, 48, 52,
            56, 64, 72, 80, 88, 96, 104, 112};

        struct ScanTables
        {
            uint8_t norm[64];
            uint8_t alt[64];
        };

        // Zig-zag / alternate scan, permuted for the IDCT below (PCSX2 make_scan_pack).
        constexpr ScanTables makeScanTables()
        {
            constexpr uint8_t scanNorm[64] = {
                0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
                12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
                35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
            constexpr uint8_t scanAlt[64] = {
                0, 8, 16, 24, 1, 9, 2, 10, 17, 25, 32, 40, 48, 56, 57, 49,
                41, 33, 26, 18, 3, 11, 4, 12, 19, 27, 34, 42, 50, 58, 35, 43,
                51, 59, 20, 28, 5, 13, 6, 14, 21, 29, 36, 44, 52, 60, 37, 45,
                53, 61, 22, 30, 7, 15, 23, 31, 38, 46, 54, 62, 39, 47, 55, 63};
            ScanTables t{};
            for (int i = 0; i < 64; ++i)
            {
                int j = scanNorm[i];
                t.norm[i] = static_cast<uint8_t>(((j & 0x36) >> 1) | ((j & 0x09) << 2));
                j = scanAlt[i];
                t.alt[i] = static_cast<uint8_t>(((j & 0x36) >> 1) | ((j & 0x09) << 2));
            }
            return t;
        }
        constexpr ScanTables kScan = makeScanTables();

        struct State
        {
            std::vector<uint8_t> in;
            size_t bitPos = 0; // absolute bit index into `in`
            bool underflow = false;

            std::vector<uint8_t> out;
            size_t outRead = 0;

            uint32_t ctrl = 0;
            uint32_t cmdData = 0;
            uint32_t top = 0;
            uint32_t command = 0xFFFFFFFFu;
            bool busy = false;

            uint8_t iq[64] = {};
            uint8_t niq[64] = {};
            uint16_t vqclut[16] = {};
            uint16_t th0 = 0;
            uint16_t th1 = 0;

            // IDEC
            int dcPred[3] = {128, 128, 128};
            int quantizerScale = 0;
            int intraDcPrecision = 0;
            bool mpeg1 = false;
            bool qScaleType = false;
            bool intraVlcFormat = false;
            bool alternateScan = false;
            bool framePredFrameDct = true;
            bool sgn = false;
            bool dte = false;
            bool ofm = false;
            int idecPhase = 0;
            int mbaCount = 0;

            // CSC
            uint32_t cscRemaining = 0;

            Stats stats;
        };

        State g;
        std::mutex g_mutex;

        // ---- bitstream ----
        size_t bitsAvailable()
        {
            const size_t total = g.in.size() * 8u;
            return g.bitPos < total ? total - g.bitPos : 0u;
        }

        uint32_t ubits(unsigned n)
        {
            if (n == 0u)
                return 0u;
            if (bitsAvailable() < n)
                g.underflow = true;
            const size_t byte = g.bitPos >> 3;
            const unsigned shift = static_cast<unsigned>(g.bitPos & 7u);
            uint64_t acc = 0;
            for (size_t i = 0; i < 5u; ++i)
                acc = (acc << 8) | (byte + i < g.in.size() ? g.in[byte + i] : 0u);
            return static_cast<uint32_t>((acc >> (40u - shift - n)) & ((1ull << n) - 1ull));
        }

        int32_t sbits(unsigned n)
        {
            const uint32_t v = ubits(n);
            return static_cast<int32_t>(v << (32u - n)) >> (32u - n);
        }

        void dumpbits(unsigned n)
        {
            if (bitsAvailable() < n)
                g.underflow = true;
            g.bitPos += n;
        }

        uint32_t getbits(unsigned n)
        {
            const uint32_t v = ubits(n);
            dumpbits(n);
            return v;
        }

        void compactInput()
        {
            const size_t consumed = g.bitPos >> 3;
            if (consumed >= (1u << 20) || (consumed > 0u && consumed == g.in.size()))
            {
                g.in.erase(g.in.begin(), g.in.begin() + static_cast<std::ptrdiff_t>(consumed));
                g.bitPos -= consumed * 8u;
            }
        }

        void emit(const uint8_t *data, size_t bytes)
        {
            g.out.insert(g.out.end(), data, data + bytes);
            g.stats.outputBytes += bytes;
        }

        // ---- IDCT (PCSX2 IDCT_Block / IDCT_Copy) ----
        constexpr int W1 = 2841;
        constexpr int W2 = 2676;
        constexpr int W3 = 2408;
        constexpr int W5 = 1609;
        constexpr int W6 = 1108;
        constexpr int W7 = 565;

        inline void butterfly(int &t0, int &t1, int w0, int w1, int d0, int d1)
        {
            const int tmp = w0 * (d0 + d1);
            t0 = tmp + (w1 - w0) * d1;
            t1 = tmp - (w1 + w0) * d0;
        }

        void idctBlock(int16_t *block)
        {
            for (int i = 0; i < 8; ++i)
            {
                int16_t *const r = block + 8 * i;
                if (!(r[1] | r[2] | r[3] | r[4] | r[5] | r[6] | r[7]))
                {
                    const int16_t v = static_cast<int16_t>(static_cast<uint16_t>(r[0] << 3));
                    for (int k = 0; k < 8; ++k)
                        r[k] = v;
                    continue;
                }
                int a0, a1, a2, a3;
                {
                    const int d0 = (r[0] << 11) + 128;
                    const int d1 = r[1];
                    const int d2 = r[2] << 11;
                    const int d3 = r[3];
                    const int t0 = d0 + d2;
                    const int t1 = d0 - d2;
                    int t2, t3;
                    butterfly(t2, t3, W6, W2, d3, d1);
                    a0 = t0 + t2;
                    a1 = t1 + t3;
                    a2 = t1 - t3;
                    a3 = t0 - t2;
                }
                int b0, b1, b2, b3;
                {
                    const int d0 = r[4];
                    const int d1 = r[5];
                    const int d2 = r[6];
                    const int d3 = r[7];
                    int t0, t1, t2, t3;
                    butterfly(t0, t1, W7, W1, d3, d0);
                    butterfly(t2, t3, W3, W5, d1, d2);
                    b0 = t0 + t2;
                    b3 = t1 + t3;
                    t0 -= t2;
                    t1 -= t3;
                    b1 = ((t0 + t1) * 181) >> 8;
                    b2 = ((t0 - t1) * 181) >> 8;
                }
                r[0] = static_cast<int16_t>((a0 + b0) >> 8);
                r[1] = static_cast<int16_t>((a1 + b1) >> 8);
                r[2] = static_cast<int16_t>((a2 + b2) >> 8);
                r[3] = static_cast<int16_t>((a3 + b3) >> 8);
                r[4] = static_cast<int16_t>((a3 - b3) >> 8);
                r[5] = static_cast<int16_t>((a2 - b2) >> 8);
                r[6] = static_cast<int16_t>((a1 - b1) >> 8);
                r[7] = static_cast<int16_t>((a0 - b0) >> 8);
            }
            for (int i = 0; i < 8; ++i)
            {
                int16_t *const c = block + i;
                int a0, a1, a2, a3;
                {
                    const int d0 = (c[8 * 0] << 11) + 65536;
                    const int d1 = c[8 * 1];
                    const int d2 = c[8 * 2] << 11;
                    const int d3 = c[8 * 3];
                    const int t0 = d0 + d2;
                    const int t1 = d0 - d2;
                    int t2, t3;
                    butterfly(t2, t3, W6, W2, d3, d1);
                    a0 = t0 + t2;
                    a1 = t1 + t3;
                    a2 = t1 - t3;
                    a3 = t0 - t2;
                }
                int b0, b1, b2, b3;
                {
                    const int d0 = c[8 * 4];
                    const int d1 = c[8 * 5];
                    const int d2 = c[8 * 6];
                    const int d3 = c[8 * 7];
                    int t0, t1, t2, t3;
                    butterfly(t0, t1, W7, W1, d3, d0);
                    butterfly(t2, t3, W3, W5, d1, d2);
                    b0 = t0 + t2;
                    b3 = t1 + t3;
                    t0 = (t0 - t2) >> 8;
                    t1 = (t1 - t3) >> 8;
                    b1 = (t0 + t1) * 181;
                    b2 = (t0 - t1) * 181;
                }
                c[8 * 0] = static_cast<int16_t>((a0 + b0) >> 17);
                c[8 * 1] = static_cast<int16_t>((a1 + b1) >> 17);
                c[8 * 2] = static_cast<int16_t>((a2 + b2) >> 17);
                c[8 * 3] = static_cast<int16_t>((a3 + b3) >> 17);
                c[8 * 4] = static_cast<int16_t>((a3 - b3) >> 17);
                c[8 * 5] = static_cast<int16_t>((a2 - b2) >> 17);
                c[8 * 6] = static_cast<int16_t>((a1 - b1) >> 17);
                c[8 * 7] = static_cast<int16_t>((a0 - b0) >> 17);
            }
        }

        void idctCopy(int16_t *block, uint8_t *dest, int stride)
        {
            idctBlock(block);
            for (int i = 0; i < 8; ++i)
            {
                for (int k = 0; k < 8; ++k)
                    dest[k] = static_cast<uint8_t>(std::clamp<int>(block[i * 8 + k], 0, 255));
                dest += stride;
            }
        }

        // ---- VLC decoding (PCSX2 IPU_MultiISA.cpp) ----
        int lumaDcDiff()
        {
            int size;
            uint32_t code = ubits(5);
            if (code < 31u)
            {
                size = DCtable.lum0[code].size;
                dumpbits(DCtable.lum0[code].len);
            }
            else
            {
                code = ubits(9) - 0x1f0u;
                size = DCtable.lum1[code & 15u].size;
                dumpbits(DCtable.lum1[code & 15u].len);
            }
            if (size == 0)
                return 0;
            int diff = static_cast<int>(getbits(static_cast<unsigned>(size)));
            if ((diff & (1 << (size - 1))) == 0)
                diff -= (1 << size) - 1;
            return diff;
        }

        int chromaDcDiff()
        {
            int size;
            uint32_t code = ubits(5);
            if (code < 31u)
            {
                size = DCtable.chrom0[code].size;
                dumpbits(DCtable.chrom0[code].len);
            }
            else
            {
                code = ubits(10) - 0x3e0u;
                size = DCtable.chrom1[code & 31u].size;
                dumpbits(DCtable.chrom1[code & 31u].len);
            }
            if (size == 0)
                return 0;
            int diff = static_cast<int>(getbits(static_cast<unsigned>(size)));
            if ((diff & (1 << (size - 1))) == 0)
                diff -= (1 << size) - 1;
            return diff;
        }

        inline void saturate(int &val)
        {
            if (static_cast<uint32_t>(val + 2048) > 4095u)
                val = (val >> 31) ^ 2047;
        }

        void getIntraBlock(int16_t *dest)
        {
            const uint8_t *scan = g.alternateScan ? kScan.alt : kScan.norm;
            const int qs = g.quantizerScale;
            const bool tableOne = g.intraVlcFormat && !g.mpeg1;
            for (int i = 1;; ++i)
            {
                if (g.underflow)
                    return;
                const uint32_t code = ubits(16);
                const DCTtab *tab = nullptr;
                if (code >= 16384u && !tableOne)
                    tab = &DCT.next[(code >> 12) - 4u];
                else if (code >= 1024u)
                    tab = tableOne ? &DCT.tab0a[(code >> 8) - 4u] : &DCT.tab0[(code >> 8) - 4u];
                else if (code >= 512u)
                    tab = tableOne ? &DCT.tab1a[(code >> 6) - 8u] : &DCT.tab1[(code >> 6) - 8u];
                else if (code >= 256u)
                    tab = &DCT.tab2[(code >> 4) - 16u];
                else if (code >= 128u)
                    tab = &DCT.tab3[(code >> 3) - 16u];
                else if (code >= 64u)
                    tab = &DCT.tab4[(code >> 2) - 16u];
                else if (code >= 32u)
                    tab = &DCT.tab5[(code >> 1) - 16u];
                else if (code >= 16u)
                    tab = &DCT.tab6[code - 16u];
                else
                    return;

                dumpbits(tab->len);
                if (tab->run == 64u) // end_of_block
                    return;
                i += (tab->run == 65u) ? static_cast<int>(getbits(6)) : tab->run;
                if (i >= 64)
                    return;

                const int j = scan[i];
                int val;
                if (tab->run == 65u) // escape
                {
                    if (!g.mpeg1)
                    {
                        val = (sbits(12) * qs * g.iq[i]) >> 4;
                        dumpbits(12);
                    }
                    else
                    {
                        val = sbits(8);
                        dumpbits(8);
                        if (!(val & 0x7f))
                            val = static_cast<int>(getbits(8)) + 2 * val;
                        val = (val * qs * g.iq[i]) >> 4;
                        val = (val + ~(val >> 31)) | 1;
                    }
                }
                else
                {
                    val = (tab->level * qs * g.iq[i]) >> 4;
                    if (g.mpeg1)
                        val = (val - 1) | 1; // oddification
                    const int bit1 = sbits(1);
                    val = (val ^ bit1) - bit1;
                    dumpbits(1);
                }
                saturate(val);
                dest[j] = static_cast<int16_t>(val);
            }
        }

        void intraBlock(int cc, uint8_t *dest, int stride)
        {
            int16_t block[64] = {};
            if (cc == 0)
                g.dcPred[0] += lumaDcDiff();
            else
                g.dcPred[cc] += chromaDcDiff();
            block[0] = static_cast<int16_t>(g.dcPred[cc] << (3 - g.intraDcPrecision));
            getIntraBlock(block);
            idctCopy(block, dest, stride);
        }

        // ---- colour conversion (PCSX2 yuv2rgb_reference, ipu_csc, ipu_dither_reference) ----
        void yuvToRgb32(const uint8_t *Y, const uint8_t *Cb, const uint8_t *Cr, uint8_t *rgba)
        {
            for (int y = 0; y < 16; ++y)
            {
                for (int x = 0; x < 16; ++x)
                {
                    const int cr = static_cast<int>(Cr[(y >> 1) * 8 + (x >> 1)]) - 128;
                    const int cb = static_cast<int>(Cb[(y >> 1) * 8 + (x >> 1)]) - 128;
                    const int lum = (0x95 * std::max(0, static_cast<int>(Y[y * 16 + x]) - 16)) >> 6;
                    const int rcr = (0xcc * cr) >> 6;
                    const int gcr = (-0x68 * cr) >> 6;
                    const int gcb = (-0x32 * cb) >> 6;
                    const int bcb = (0x102 * cb) >> 6;
                    uint8_t *p = rgba + (y * 16 + x) * 4;
                    p[0] = static_cast<uint8_t>(std::clamp((lum + rcr + 1) >> 1, 0, 255));
                    p[1] = static_cast<uint8_t>(std::clamp((lum + gcr + gcb + 1) >> 1, 0, 255));
                    p[2] = static_cast<uint8_t>(std::clamp((lum + bcb + 1) >> 1, 0, 255));
                    p[3] = 0x80;
                }
            }
            if (g.th0 > 0)
            {
                for (int i = 0; i < 256; ++i)
                {
                    uint8_t *p = rgba + i * 4;
                    if (p[0] < g.th0 && p[1] < g.th0 && p[2] < g.th0)
                        std::memset(p, 0, 4);
                    else if (p[0] < g.th1 && p[1] < g.th1 && p[2] < g.th1)
                        p[3] = 0x40;
                }
            }
            else if (g.th1 > 0)
            {
                for (int i = 0; i < 256; ++i)
                {
                    uint8_t *p = rgba + i * 4;
                    if (p[0] < g.th1 && p[1] < g.th1 && p[2] < g.th1)
                        p[3] = 0x40;
                }
            }
        }

        void emitMacroblock(uint8_t *rgba, bool sgn, bool ofm, bool dte)
        {
            if (sgn)
            {
                for (int i = 0; i < 256; ++i)
                {
                    rgba[i * 4 + 0] ^= 0x80;
                    rgba[i * 4 + 1] ^= 0x80;
                    rgba[i * 4 + 2] ^= 0x80;
                }
            }
            if (!ofm)
            {
                emit(rgba, 1024u);
                return;
            }
            static const int kDither[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};
            uint8_t rgb16[512];
            for (int i = 0; i < 16; ++i)
            {
                for (int j = 0; j < 16; ++j)
                {
                    const uint8_t *p = rgba + (i * 16 + j) * 4;
                    const int d = dte ? kDither[i & 3][j & 3] : 0;
                    const int r = std::clamp(p[0] + d, 0, 255) >> 3;
                    const int gg = std::clamp(p[1] + d, 0, 255) >> 3;
                    const int b = std::clamp(p[2] + d, 0, 255) >> 3;
                    const uint16_t v = static_cast<uint16_t>(r | (gg << 5) | (b << 10) | ((p[3] == 0x40) ? 0x8000 : 0));
                    rgb16[(i * 16 + j) * 2 + 0] = static_cast<uint8_t>(v);
                    rgb16[(i * 16 + j) * 2 + 1] = static_cast<uint8_t>(v >> 8);
                }
            }
            emit(rgb16, sizeof(rgb16));
        }

        void resetDcPredictors()
        {
            g.dcPred[0] = g.dcPred[1] = g.dcPred[2] = 128 << g.intraDcPrecision;
        }

        // One intra macroblock (IDEC). Returns false when the input ran out (state rolled back by the caller).
        bool idecMacroblock()
        {
            int modes = 0;
            {
                const uint32_t v = ubits(2);
                if (v != 0u)
                {
                    const MBtab &tab = MB_I[v >> 1];
                    dumpbits(tab.len);
                    modes = tab.modes;
                    if (!g.framePredFrameDct)
                        modes |= static_cast<int>(getbits(1)) * DCT_TYPE_INTERLACED;
                }
            }
            if (modes & MACROBLOCK_QUANT)
            {
                const uint32_t qsc = getbits(5);
                g.quantizerScale = g.qScaleType ? kNonLinearQuantizerScale[qsc] : static_cast<int>(qsc << 1);
            }

            uint8_t Y[256] = {};
            uint8_t Cb[64] = {};
            uint8_t Cr[64] = {};
            const bool interlaced = (modes & DCT_TYPE_INTERLACED) != 0;
            const int dctOffset = interlaced ? 16 : 16 * 8;
            const int dctStride = interlaced ? 32 : 16;
            intraBlock(0, Y, dctStride);
            intraBlock(0, Y + 8, dctStride);
            intraBlock(0, Y + dctOffset, dctStride);
            intraBlock(0, Y + dctOffset + 8, dctStride);
            intraBlock(1, Cb, 8);
            intraBlock(2, Cr, 8);
            if (g.underflow)
                return false;

            uint8_t rgba[1024];
            yuvToRgb32(Y, Cb, Cr, rgba);
            emitMacroblock(rgba, g.sgn, g.ofm, g.dte);
            ++g.stats.macroblocks;
            return true;
        }

        // IDEC as a resumable state machine: 0 macroblock, 1 address increment, 2 end of slice.
        bool runIdec()
        {
            for (;;)
            {
                if (g.idecPhase == 0)
                {
                    const size_t savedPos = g.bitPos;
                    int savedDc[3] = {g.dcPred[0], g.dcPred[1], g.dcPred[2]};
                    const int savedQs = g.quantizerScale;
                    const size_t savedOut = g.out.size();
                    g.underflow = false;
                    if (!idecMacroblock())
                    {
                        g.bitPos = savedPos;
                        std::memcpy(g.dcPred, savedDc, sizeof(savedDc));
                        g.quantizerScale = savedQs;
                        g.out.resize(savedOut);
                        g.underflow = false;
                        return false;
                    }
                    g.mbaCount = 0;
                    g.idecPhase = 1;
                }
                if (g.idecPhase == 1)
                {
                    const size_t savedPos = g.bitPos;
                    const int savedCount = g.mbaCount;
                    g.underflow = false;
                    const MBAtab *mba = nullptr;
                    bool endOfSlice = false;
                    for (;;)
                    {
                        const uint32_t code = ubits(16);
                        if (code >= 0x1000u)
                        {
                            mba = &MBA.mba5[ubits(5) - 2u];
                            break;
                        }
                        if (code >= 0x0300u)
                        {
                            mba = &MBA.mba11[ubits(11) - 24u];
                            break;
                        }
                        const uint32_t c11 = ubits(11);
                        if (c11 == 8u) // macroblock_escape
                        {
                            g.mbaCount += 33;
                            dumpbits(11);
                            if (g.underflow)
                                break;
                            continue;
                        }
                        if (c11 == 15u) // macroblock_stuffing
                        {
                            dumpbits(11);
                            if (g.underflow)
                                break;
                            continue;
                        }
                        endOfSlice = true;
                        break;
                    }
                    if (g.underflow)
                    {
                        g.bitPos = savedPos;
                        g.mbaCount = savedCount;
                        g.underflow = false;
                        return false;
                    }
                    if (endOfSlice)
                    {
                        g.idecPhase = 2;
                    }
                    else
                    {
                        dumpbits(mba->len);
                        g.mbaCount += mba->mba;
                        if (g.mbaCount)
                            resetDcPredictors();
                        g.idecPhase = 0;
                        continue;
                    }
                }
                // end of slice: find the next start code, then load TOP
                {
                    const size_t savedPos = g.bitPos;
                    g.ctrl &= ~kCtrlScd;
                    if (bitsAvailable() < 8u)
                        return false;
                    if (ubits(8) == 0u)
                    {
                        g.bitPos = (g.bitPos + 7u) & ~static_cast<size_t>(7u);
                        for (;;)
                        {
                            if (bitsAvailable() < 24u)
                            {
                                g.bitPos = savedPos;
                                return false;
                            }
                            const uint32_t startCheck = ubits(24);
                            if (startCheck != 0u)
                            {
                                if (startCheck == 1u)
                                    g.ctrl |= kCtrlScd;
                                else
                                    g.ctrl |= kCtrlEcd;
                                break;
                            }
                            g.bitPos += 8u;
                        }
                    }
                    if (bitsAvailable() < 32u)
                    {
                        g.bitPos = savedPos;
                        return false;
                    }
                    g.top = ubits(32);
                    return true;
                }
            }
        }

        bool runCsc()
        {
            const bool ofm = (g.command >> 27) & 1u;
            const bool dte = (g.command >> 26) & 1u;
            while (g.cscRemaining != 0u)
            {
                if (bitsAvailable() < 384u * 8u)
                    return false;
                uint8_t Y[256], Cb[64], Cr[64];
                for (uint8_t &b : Y)
                    b = static_cast<uint8_t>(getbits(8));
                for (uint8_t &b : Cb)
                    b = static_cast<uint8_t>(getbits(8));
                for (uint8_t &b : Cr)
                    b = static_cast<uint8_t>(getbits(8));
                uint8_t rgba[1024];
                yuvToRgb32(Y, Cb, Cr, rgba);
                emitMacroblock(rgba, false, ofm, dte);
                --g.cscRemaining;
            }
            return true;
        }

        // Runs the current command as far as the input allows; clears busy when it completes.
        void run()
        {
            if (!g.busy)
                return;
            dodperf::Scope ipuRun(dodperf::IpuRun);
            bool done = false;
            switch (g.command >> 28)
            {
            case IDEC:
                done = runIdec();
                break;
            case FDEC:
                if (bitsAvailable() >= 32u)
                {
                    g.cmdData = ubits(32);
                    g.top = g.cmdData;
                    done = true;
                }
                break;
            case SETIQ:
                if (bitsAvailable() >= 512u)
                {
                    uint8_t *table = ((g.command >> 27) & 1u) ? g.niq : g.iq;
                    for (int i = 0; i < 64; ++i)
                        table[i] = static_cast<uint8_t>(getbits(8));
                    done = true;
                }
                break;
            case SETVQ:
                if (bitsAvailable() >= 256u)
                {
                    for (uint16_t &v : g.vqclut)
                    {
                        const uint32_t lo = getbits(8);
                        const uint32_t hi = getbits(8);
                        v = static_cast<uint16_t>(lo | (hi << 8));
                    }
                    done = true;
                }
                break;
            case CSC:
                done = runCsc();
                break;
            default:
                done = true;
                break;
            }
            if (done)
            {
                g.busy = false;
                compactInput();
            }
        }

        void softReset()
        {
            g.in.clear();
            g.bitPos = 0;
            g.out.clear();
            g.outRead = 0;
            g.th0 = g.th1 = 0;
            if (g.intraDcPrecision == 0)
                resetDcPredictors();
            g.ctrl &= 0x7F33F00u;
            g.top = 0;
            g.command = 0xFFFFFFFFu;
            g.busy = false;
            g.cmdData = 0;
        }

        void writeCommand(uint32_t value)
        {
            ++g.stats.commands;
            g.ctrl &= ~(kCtrlEcd | kCtrlScd);
            g.command = value;
            const uint32_t fb = value & 0x3Fu;
            switch (value >> 28)
            {
            case BCLR:
                g.in.clear();
                g.bitPos = value & 0x7Fu;
                g.busy = false;
                return;
            case SETTH:
                g.th0 = static_cast<uint16_t>(value & 0x1FFu);
                g.th1 = static_cast<uint16_t>((value >> 16) & 0x1FFu);
                g.busy = false;
                return;
            case IDEC:
                dumpbits(fb);
                g.ctrl = (g.ctrl & ~(7u << 24)) | (1u << 24); // PCT = I picture
                g.mpeg1 = (g.ctrl >> 23) & 1u;
                g.qScaleType = (g.ctrl >> 22) & 1u;
                g.intraVlcFormat = (g.ctrl >> 21) & 1u;
                g.alternateScan = (g.ctrl >> 20) & 1u;
                g.intraDcPrecision = static_cast<int>((g.ctrl >> 16) & 3u);
                g.quantizerScale = static_cast<int>((value >> 16) & 0x1Fu); // as PCSX2 (raw QSC until a QUANT macroblock)
                g.framePredFrameDct = ((value >> 24) & 1u) == 0u;
                g.sgn = (value >> 25) & 1u;
                g.dte = (value >> 26) & 1u;
                g.ofm = (value >> 27) & 1u;
                resetDcPredictors();
                g.top = 0;
                g.idecPhase = 0;
                g.mbaCount = 0;
                break;
            case FDEC:
            case SETIQ:
            case VDEC:
            case BDEC:
                dumpbits(fb);
                break;
            case CSC:
                g.cscRemaining = value & 0x7FFu;
                break;
            default:
                break;
            }
            g.underflow = false;
            g.busy = true;
            const uint32_t cmd = value >> 28;
            if (cmd == BDEC || cmd == VDEC || cmd == PACK)
            {
                static int s_warned = 0;
                if (s_warned++ < 8)
                    std::fprintf(stderr, "[ipu] command %u (0x%08X) is not emulated yet\n", cmd, value);
                ++g.stats.errors;
                g.cmdData = 0;
                g.busy = false;
                return;
            }
            run();
        }
    }

    void reset()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g = State{};
    }

    uint32_t read32(uint32_t offset)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const size_t inQw = bitsAvailable() / 128u;
        const size_t outQw = (g.out.size() - g.outRead) / 16u;
        switch (offset & 0xFFu)
        {
        case 0x00:
        {
            const uint32_t cmd = g.command >> 28;
            if (cmd != FDEC && cmd != VDEC && bitsAvailable() >= 32u)
                g.cmdData = ubits(32);
            return g.cmdData;
        }
        case 0x04:
            return g.busy ? 0x80000000u : 0u;
        case 0x10:
        {
            uint32_t v = g.ctrl & ~0xFFu & ~(1u << 31);
            v |= static_cast<uint32_t>(std::min<size_t>(inQw, 8u));
            v |= static_cast<uint32_t>(std::min<size_t>(outQw, 8u)) << 4;
            if (g.busy)
                v |= 1u << 31;
            return v;
        }
        case 0x20:
            return static_cast<uint32_t>(g.bitPos & 0x7Fu) |
                   (static_cast<uint32_t>(std::min<size_t>(inQw, 8u)) << 8) |
                   (static_cast<uint32_t>(std::min<size_t>(inQw, 2u)) << 16);
        case 0x30:
            return g.top;
        case 0x34:
            return g.busy ? 0x80000000u : 0u;
        default:
            return 0u;
        }
    }

    void write32(uint32_t offset, uint32_t value)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        switch (offset & 0xFFu)
        {
        case 0x00:
            writeCommand(value);
            break;
        case 0x10:
            g.ctrl = (value & 0x47f30000u) | (g.ctrl & 0x8000ffffu);
            if (((g.ctrl >> 16) & 3u) == 3u)
                g.ctrl = (g.ctrl & ~(3u << 16)) | (1u << 16);
            if (g.ctrl & kCtrlRst)
            {
                g.ctrl &= ~kCtrlRst;
                softReset();
            }
            break;
        default:
            break;
        }
    }

    void pushInput(const uint8_t *data, size_t bytes)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        {
            // [DoD] DOD_IPU_INDUMP=<file>: the IPU input stream as it arrives (first 8 MB).
            static FILE *dump = []() -> FILE * { const char *v = std::getenv("DOD_IPU_INDUMP"); return v ? std::fopen(v, "wb") : nullptr; }();
            static size_t dumped = 0;
            if (dump && dumped < (8u << 20)) { std::fwrite(data, 1, bytes, dump); std::fflush(dump); dumped += bytes; }
        }
        g.in.insert(g.in.end(), data, data + bytes);
        g.stats.inputBytes += bytes;
        run();
    }

    size_t outputBytes()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g.out.size() - g.outRead;
    }

    size_t popOutput(uint8_t *dst, size_t bytes)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const size_t n = std::min(bytes, g.out.size() - g.outRead);
        std::memcpy(dst, g.out.data() + g.outRead, n);
        g.outRead += n;
        if (g.outRead == g.out.size())
        {
            g.out.clear();
            g.outRead = 0;
        }
        return n;
    }

    Stats stats()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g.stats;
    }
}
