// [DoD] spu2_diff: offline differential test of ps2xIOP's SPU2 (iop_spu2.cpp, compiled unchanged, driven only through
// its registers and sound RAM) against PCSX2's own SPU2 code (pcsx2_side.cpp) on the same input:
//  1. reverb: core 1 set up as DoD sets it in room 0x386 (Studio Large; values from DOD_IO_CENSUS), a test signal
//     through the sound data input (dry and wet), compared sample by sample at the core 1 output and over the whole
//     effects area of sound RAM once a second. FxEnable is switched off for one second in the middle.
//  2. Gaussian interpolation: one voice playing known ADPCM data at several pitches, compared with PCSX2's
//     GetVoiceValues formula and table.
// Usage: spu2_diff.exe [seconds]. Exit code 1 if anything differs. SPU2DIFF_DEBUG=1 also prints where each side has
// written the effects area after samples 99 and 100. spu2_diff.exe --ir <file> [evol hex]: see writeImpulseResponse.
// Build: build.cmd (three variants: PCSX2 reference / SSE4.1 / AVX2 resampling).
#include "iop_spu2.h"
#include "pcsx2_side.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

using ps2x::iop::detail::IopSpu2;

namespace
{
    int32_t clamp16(int32_t v) { return v < -32768 ? -32768 : (v > 32767 ? 32767 : v); }

    struct RegVal
    {
        uint32_t reg;
        uint16_t value;
    };

    // Core 1 reverb registers as DoD writes them (room 0x386, DOD_IO_CENSUS), as core 0 offsets: the core area is at
    // +0x400 for core 1, the master area at +0x28.
    const RegVal kCoreRegs[] = {
        {0x2E0, 0x000F}, {0x2E2, 0xC810}, // ESA = 0xFC810
        {0x2E4, 0}, {0x2E6, 0x038C}, {0x2E8, 0}, {0x2EA, 0x02A4}, {0x2EC, 0}, {0x2EE, 0x37EC}, {0x2F0, 0}, {0x2F2, 0x2D60},
        {0x2F4, 0}, {0x2F6, 0x3424}, {0x2F8, 0}, {0x2FA, 0x28F0}, {0x2FC, 0}, {0x2FE, 0x2F64}, {0x300, 0}, {0x302, 0x25CC},
        {0x304, 0}, {0x306, 0x2D64}, {0x308, 0}, {0x30A, 0x2368}, {0x30C, 0}, {0x30E, 0x2364}, {0x310, 0}, {0x312, 0x17A4},
        {0x314, 0}, {0x316, 0x1FB0}, {0x318, 0}, {0x31A, 0x12C0}, {0x31C, 0}, {0x31E, 0x1BBC}, {0x320, 0}, {0x322, 0x0F48},
        {0x324, 0}, {0x326, 0x17A8}, {0x328, 0}, {0x32A, 0x0C74}, {0x32C, 0}, {0x32E, 0x0C70}, {0x330, 0}, {0x332, 0x08E0},
        {0x334, 0}, {0x336, 0x0550}, {0x338, 0}, {0x33A, 0x02A8},
        {0x33C, 0x000F}, // EEA = 0xFFFFF
    };
    const RegVal kMasterRegs[] = {
        {0x774, 0x6F60}, {0x776, 0x4FA8}, {0x778, 0xBCE0}, {0x77A, 0x4510}, {0x77C, 0xBEF0},
        {0x77E, 0xA680}, {0x780, 0x5680}, {0x782, 0x52C0}, {0x784, 0x8000}, {0x786, 0x8000},
    };
    constexpr uint32_t kEsa = 0xFC810u;
    constexpr uint32_t kAreaHalfwords = 0x100000u - kEsa;

    // Test signal, independent per channel: 0.25 s segments of silence, noise at three levels, a sine sweep, clicks
    // and a full-scale square (the input saturates in the mixer and the reverb).
    struct Signal
    {
        std::mt19937 rng{12345u};
        int32_t next(uint32_t n, int channel)
        {
            const uint32_t segment = (n / 12000u + static_cast<uint32_t>(channel) * 3u) % 7u;
            const double t = static_cast<double>(n % 12000u) / 48000.0;
            switch (segment)
            {
            case 0:
                return 0;
            case 1:
                return static_cast<int32_t>(rng() % 2001u) - 1000;
            case 2:
                return static_cast<int32_t>(rng() % 16001u) - 8000;
            case 3:
                return static_cast<int32_t>(rng() % 65536u) - 32768;
            case 4:
                return static_cast<int32_t>(20000.0 * std::sin(2.0 * 3.14159265358979 * (100.0 + 40000.0 * t) * t));
            case 5:
                return (n % 4800u) == 0u ? 30000 : 0;
            default:
                return ((n / 40u) & 1u) != 0u ? 32767 : -32768;
            }
        }
    };

    void setTsa(IopSpu2 &spu, uint32_t address)
    {
        spu.write16(0x400u + 0x1A8u, static_cast<uint16_t>(address >> 16));
        spu.write16(0x400u + 0x1AAu, static_cast<uint16_t>(address));
    }

    int testReverb(uint32_t seconds)
    {
        IopSpu2 spu;
        pcsx2side::reset();
        for (const RegVal &r : kCoreRegs)
        {
            spu.write16(0x400u + r.reg, r.value);
            pcsx2side::writeReg(1, r.reg, r.value);
        }
        for (const RegVal &r : kMasterRegs)
        {
            spu.write16(r.reg + 0x28u, r.value);
            pcsx2side::writeReg(1, r.reg, r.value);
        }
        const int16_t evolL = 0x6000, evolR = static_cast<int16_t>(0xC000); // a negative one too
        spu.write16(0x78Cu, static_cast<uint16_t>(evolL));
        spu.write16(0x78Eu, static_cast<uint16_t>(evolR));
        spu.write16(0x788u, 0x3FFF); // MVOL
        spu.write16(0x78Au, 0x3FFF);
        spu.write16(0x794u, 0x7FFF); // BVOL
        spu.write16(0x796u, 0x7FFF);
        spu.write16(0x598u, 0x00F0); // MMIX: sound data input dry and wet
        spu.write16(0x59Au, 0xC081); // ATTR as DoD: effects on
        pcsx2side::setFxEnable(1, true);
        const int32_t mvol = 0x7FFE; // MVOL 0x3FFF in both (SignExtend16(0x3FFF << 1))

        Signal signal;
        const uint32_t total = seconds * 48000u;
        uint64_t mismatches = 0, ramMismatches = 0, ramChecks = 0;
        int32_t maxDiff = 0;
        uint64_t firstMismatch = ~0ull;
        double sumSq = 0.0, sumSqRef = 0.0;
        std::vector<uint16_t> area(kAreaHalfwords);
        for (uint32_t n = 0; n < total; ++n)
        {
            if (n == total / 2u)
            {
                spu.write16(0x59Au, 0xC001);
                pcsx2side::setFxEnable(1, false);
            }
            else if (n == total / 2u + 48000u)
            {
                spu.write16(0x59Au, 0xC081);
                pcsx2side::setFxEnable(1, true);
            }
            const int32_t l = signal.next(n, 0), r = signal.next(n, 1);
            const uint32_t pos = n & 0x1FFu;
            setTsa(spu, 0x2400u + pos);
            spu.write16(0x400u + 0x1ACu, static_cast<uint16_t>(l));
            setTsa(spu, 0x2600u + pos);
            spu.write16(0x400u + 0x1ACu, static_cast<uint16_t>(r));
            int16_t out[2] = {0, 0};
            if (spu.generate(out, 1u) != 1u)
            {
                std::printf("generate did not produce a sample\n");
                return 1;
            }

            // PCSX2 spu2Mix / MixCore for this set-up: Input = ApplyVolume(ReadInput, InpVol), TD = TW = Input,
            // Out = ApplyVolume(clamp_mix(TD + ApplyVolume(DoReverb(TW), FxVol)), MasterVol).
            const int32_t inL = (l * 0x7FFF) >> 15, inR = (r * 0x7FFF) >> 15;
            int32_t rvL = 0, rvR = 0;
            pcsx2side::doReverb(1, n, inL, inR, rvL, rvR);
            const int32_t refL = (clamp16(inL + ((rvL * evolL) >> 15)) * mvol) >> 15;
            const int32_t refR = (clamp16(inR + ((rvR * evolR) >> 15)) * mvol) >> 15;
            const int32_t dl = out[0] - refL, dr = out[1] - refR;
            if (dl != 0 || dr != 0)
            {
                if (mismatches == 0u)
                    firstMismatch = n;
                ++mismatches;
                maxDiff = std::max(maxDiff, std::max(std::abs(dl), std::abs(dr)));
            }
            sumSq += static_cast<double>(dl) * dl + static_cast<double>(dr) * dr;
            const int32_t wetL = (rvL * evolL) >> 15, wetR = (rvR * evolR) >> 15;
            sumSqRef += static_cast<double>(wetL) * wetL + static_cast<double>(wetR) * wetR;

            if (std::getenv("SPU2DIFF_DEBUG") && (n == 99u || n == 100u))
            {
                // Where each side has written so far (first nonzero halfwords of the effects area).
                setTsa(spu, kEsa);
                spu.dmaRead(1, reinterpret_cast<uint8_t *>(area.data()), kAreaHalfwords * 2u);
                const int16_t *mem = pcsx2side::mem();
                std::printf("  after sample %u:\n   ours:", n);
                for (uint32_t i = 0, shown = 0; i < kAreaHalfwords && shown < 12u; ++i)
                    if (area[i] != 0u && ++shown)
                        std::printf(" +%04X=%d", i, static_cast<int16_t>(area[i]));
                std::vector<uint16_t> all(0x100000u);
                setTsa(spu, 0u);
                spu.dmaRead(1, reinterpret_cast<uint8_t *>(all.data()), 0x200000u);
                std::printf("\n   ours, anywhere outside the input area:");
                for (uint32_t i = 0, shown = 0; i < 0x100000u && shown < 12u; ++i)
                    if (all[i] != 0u && (i < 0x2400u || i >= 0x2800u) && ++shown)
                        std::printf(" %05X=%d", i, static_cast<int16_t>(all[i]));
                std::printf("\n  PCSX2:");
                for (uint32_t i = 0, shown = 0; i < kAreaHalfwords && shown < 12u; ++i)
                    if (mem[kEsa + i] != 0 && ++shown)
                        std::printf(" +%04X=%d", i, mem[kEsa + i]);
                std::printf("\n");
            }
            if ((n % 48000u) == 47999u)
            {
                setTsa(spu, kEsa);
                spu.dmaRead(1, reinterpret_cast<uint8_t *>(area.data()), kAreaHalfwords * 2u);
                const int16_t *mem = pcsx2side::mem();
                uint32_t bad = 0;
                for (uint32_t i = 0; i < kAreaHalfwords; ++i)
                    bad += area[i] != static_cast<uint16_t>(mem[kEsa + i]) ? 1u : 0u;
                ++ramChecks;
                if (bad != 0u)
                    ++ramMismatches;
                if (bad != 0u && ramMismatches <= 3u)
                    std::printf("  effects area differs at %.0f s: %u of %u halfwords\n", (n + 1) / 48000.0, bad, kAreaHalfwords);
            }
        }
        const double rmsDiff = std::sqrt(sumSq / (2.0 * total)), rmsWet = std::sqrt(sumSqRef / (2.0 * total));
        std::printf("reverb (%s): %u s, %llu of %u samples differ (first at %lld, max |diff| %d, rms diff %.3f vs reverb rms %.1f); "
                    "effects area differs in %llu of %llu checks\n",
                    pcsx2side::variant(), seconds, static_cast<unsigned long long>(mismatches), total,
                    mismatches ? static_cast<long long>(firstMismatch) : -1ll, maxDiff, rmsDiff, rmsWet,
                    static_cast<unsigned long long>(ramMismatches), static_cast<unsigned long long>(ramChecks));
        return mismatches == 0u && ramMismatches == 0u ? 0 : 1;
    }

    int testGauss(uint16_t pitch, uint32_t frames)
    {
        IopSpu2 spu;
        // 200 ADPCM blocks at 0x10000, shift 0 / filter 0 (sample = nibble << 12), looping to the first.
        constexpr uint32_t kBlocks = 200u, kBase = 0x10000u;
        std::mt19937 rng(pitch);
        std::vector<int16_t> stream;
        setTsa(spu, kBase);
        for (uint32_t b = 0; b < kBlocks; ++b)
        {
            const uint16_t flags = b == 0u ? 0x04u : (b == kBlocks - 1u ? 0x03u : 0x00u);
            spu.write16(0x400u + 0x1ACu, static_cast<uint16_t>(flags << 8));
            uint16_t nibbles[28];
            for (uint32_t i = 0; i < 28u; ++i)
            {
                nibbles[i] = static_cast<uint16_t>(rng() & 0xFu);
                stream.push_back(static_cast<int16_t>(static_cast<uint16_t>(nibbles[i] << 12)));
            }
            for (uint32_t w = 0; w < 7u; ++w)
                spu.write16(0x400u + 0x1ACu, static_cast<uint16_t>(nibbles[w * 4] | (nibbles[w * 4 + 1] << 4) |
                                                                  (nibbles[w * 4 + 2] << 8) | (nibbles[w * 4 + 3] << 12)));
        }
        spu.write16(0x400u, 0x3FFF);           // voice 0 VOLL
        spu.write16(0x402u, 0x2000);           // VOLR
        spu.write16(0x404u, pitch);            // PITCH
        spu.write16(0x406u, 0x00FF);           // ADSR1: fastest attack, sustain level max
        spu.write16(0x408u, 0x1FC0);           // ADSR2: sustain rate 0x7F (holds)
        spu.write16(0x5C0u, kBase >> 16);      // SSA
        spu.write16(0x5C2u, kBase & 0xFFFFu);
        spu.write16(0x588u, 1u);               // VMIXL
        spu.write16(0x590u, 1u);               // VMIXR
        spu.write16(0x598u, 0x0C00u);          // MMIX: dry voices
        spu.write16(0x788u, 0x3FFF);
        spu.write16(0x78Au, 0x3FFF);
        spu.write16(0x5A0u, 1u);               // KON voice 0
        const int32_t volL = 0x7FFE, volR = 0x4000, mvol = 0x7FFE;

        uint64_t mismatches = 0;
        int32_t maxDiff = 0;
        for (uint32_t n = 0; n < frames; ++n)
        {
            int16_t out[2] = {0, 0};
            (void)spu.generate(out, 1u);
            const int32_t env = spu.read16(0x40Au); // ENVX after the sample: the level it used (PCSX2 steps it first)
            const uint64_t p = static_cast<uint64_t>(n) * pitch;
            const uint64_t index = p >> 12;
            int16_t fifo[4];
            for (uint32_t k = 0; k < 4u; ++k)
            {
                const uint64_t s = index + k; // three zeros, then the looping stream
                fifo[k] = s < 3u ? 0 : stream[(s - 3u) % stream.size()];
            }
            const int32_t value = (pcsx2side::voiceValue(static_cast<uint32_t>(p & 0xFFFu), fifo) * env) >> 15;
            const int32_t refL = clamp16((clamp16((value * volL) >> 15) * mvol) >> 15);
            const int32_t refR = clamp16((clamp16((value * volR) >> 15) * mvol) >> 15);
            const int32_t d = std::max(std::abs(out[0] - refL), std::abs(out[1] - refR));
            if (d != 0)
            {
                if (mismatches < 3u)
                    std::printf("  pitch %04X sample %u: ours %d/%d, PCSX2 formula %d/%d\n", pitch, n, out[0], out[1], refL, refR);
                ++mismatches;
                maxDiff = std::max(maxDiff, d);
            }
        }
        std::printf("gaussian interpolation, pitch %04X: %llu of %u samples differ (max |diff| %d)\n", pitch,
                    static_cast<unsigned long long>(mismatches), frames, maxDiff);
        return mismatches == 0u ? 0 : 1;
    }
}

namespace
{
    // 3. ADSR: 48 voices (both cores) at a time, each with its own ADSR1/ADSR2, keyed on at sample 0 and off at
    //    `keyOff`, playing a silent looping block; ENVX after each sample against PCSX2's V_ADSR (ADSR.cpp).
    // 4. Volume slides: 48 voice VOLL registers at a time, set to a fixed volume and then to a slide mode; VOLXL after
    //    each sample against PCSX2's V_VolumeSlide::Update.
    uint32_t voiceReg(int core, int voice, uint32_t reg) { return static_cast<uint32_t>(core) * 0x400u + voice * 0x10u + reg * 2u; }

    int testAdsr(const std::vector<std::pair<uint16_t, uint16_t>> &configs, uint32_t keyOff, uint32_t samples, const char *what)
    {
        uint64_t bad = 0, total = 0;
        uint32_t reported = 0;
        std::vector<int32_t> ref(samples);
        for (size_t first = 0; first < configs.size(); first += 48u)
        {
            IopSpu2 spu;
            setTsa(spu, 0x20000u);
            spu.write16(0x400u + 0x1ACu, 0x0700u); // one silent block, loop start + end + repeat
            for (int i = 0; i < 7; ++i)
                spu.write16(0x400u + 0x1ACu, 0u);
            const size_t count = std::min<size_t>(48u, configs.size() - first);
            for (size_t k = 0; k < count; ++k)
            {
                const int c = static_cast<int>(k / 24u), v = static_cast<int>(k % 24u);
                spu.write16(voiceReg(c, v, 0), 0x3FFF);
                spu.write16(voiceReg(c, v, 2), 0x1000);
                spu.write16(voiceReg(c, v, 3), configs[first + k].first);
                spu.write16(voiceReg(c, v, 4), configs[first + k].second);
                spu.write16(static_cast<uint32_t>(c) * 0x400u + 0x1C0u + v * 0xCu, 0x0002u);
                spu.write16(static_cast<uint32_t>(c) * 0x400u + 0x1C2u + v * 0xCu, 0x0000u);
            }
            std::vector<std::vector<int32_t>> ours(count, std::vector<int32_t>(samples));
            for (int c = 0; c < 2; ++c)
            {
                spu.write16(static_cast<uint32_t>(c) * 0x400u + 0x1A0u, 0xFFFFu);
                spu.write16(static_cast<uint32_t>(c) * 0x400u + 0x1A2u, 0x00FFu);
            }
            for (uint32_t n = 0; n < samples; ++n)
            {
                if (n == keyOff)
                    for (int c = 0; c < 2; ++c)
                    {
                        spu.write16(static_cast<uint32_t>(c) * 0x400u + 0x1A4u, 0xFFFFu);
                        spu.write16(static_cast<uint32_t>(c) * 0x400u + 0x1A6u, 0x00FFu);
                    }
                int16_t out[2];
                (void)spu.generate(out, 1u);
                for (size_t k = 0; k < count; ++k)
                    ours[k][n] = spu.read16(voiceReg(static_cast<int>(k / 24u), static_cast<int>(k % 24u), 5));
            }
            for (size_t k = 0; k < count; ++k)
            {
                pcsx2side::adsrRun(configs[first + k].first, configs[first + k].second, keyOff, samples, ref.data());
                ++total;
                for (uint32_t n = 0; n < samples; ++n)
                    if (ours[k][n] != ref[n])
                    {
                        ++bad;
                        if (reported++ < 8u)
                            std::printf("  %s ADSR1 %04X ADSR2 %04X: first difference at sample %u: ours %d, PCSX2 %d\n", what,
                                        configs[first + k].first, configs[first + k].second, n, ours[k][n], ref[n]);
                        break;
                    }
            }
        }
        std::printf("ADSR %s: %llu of %llu settings differ (%u samples each, key off at %u)\n", what,
                    static_cast<unsigned long long>(bad), static_cast<unsigned long long>(total), samples, keyOff);
        return bad == 0u ? 0 : 1;
    }

    int testVolumeSlides(uint32_t samples)
    {
        std::vector<std::pair<uint16_t, uint16_t>> configs; // (fixed start, slide)
        for (uint32_t mode = 0; mode < 8u; ++mode)
            for (uint32_t rate = 0; rate < 128u; ++rate)
                for (const uint16_t start : {0x2000u, 0x6000u, 0x0000u})
                    configs.emplace_back(start, static_cast<uint16_t>(0x8000u | (mode << 12) | rate));
        uint64_t bad = 0;
        uint32_t reported = 0;
        std::vector<int32_t> ref(samples);
        for (size_t first = 0; first < configs.size(); first += 48u)
        {
            IopSpu2 spu;
            const size_t count = std::min<size_t>(48u, configs.size() - first);
            for (size_t k = 0; k < count; ++k)
            {
                const int c = static_cast<int>(k / 24u), v = static_cast<int>(k % 24u);
                spu.write16(voiceReg(c, v, 0), configs[first + k].first);
                spu.write16(voiceReg(c, v, 0), configs[first + k].second);
            }
            std::vector<std::vector<int32_t>> ours(count, std::vector<int32_t>(samples));
            for (uint32_t n = 0; n < samples; ++n)
            {
                int16_t out[2];
                (void)spu.generate(out, 1u);
                for (size_t k = 0; k < count; ++k)
                    ours[k][n] = static_cast<int16_t>(spu.read16(voiceReg(static_cast<int>(k / 24u), static_cast<int>(k % 24u), 6)));
            }
            for (size_t k = 0; k < count; ++k)
            {
                pcsx2side::volumeRun(configs[first + k].first, configs[first + k].second, samples, ref.data());
                for (uint32_t n = 0; n < samples; ++n)
                    if (ours[k][n] != ref[n])
                    {
                        ++bad;
                        if (reported++ < 8u)
                            std::printf("  volume start %04X slide %04X: first difference at sample %u: ours %d, PCSX2 %d\n",
                                        configs[first + k].first, configs[first + k].second, n, ours[k][n], ref[n]);
                        break;
                    }
            }
        }
        std::printf("volume slides: %llu of %zu settings differ (%u samples each)\n", static_cast<unsigned long long>(bad),
                    configs.size(), samples);
        return bad == 0u ? 0 : 1;
    }

    int testEnvelopes()
    {
        int failed = 0;
        std::vector<std::pair<uint16_t, uint16_t>> attack, decay, sustain, release;
        for (uint32_t rate = 0; rate < 128u; ++rate)
            for (uint32_t mode = 0; mode < 2u; ++mode)
                attack.emplace_back(static_cast<uint16_t>((mode << 15) | (rate << 8) | 0x00FFu), 0x1FC0u); // slow decay, hold
        for (uint32_t shift = 0; shift < 16u; ++shift)
            for (uint32_t level = 0; level < 16u; ++level)
                decay.emplace_back(static_cast<uint16_t>((shift << 4) | level), 0x1FC0u);
        for (uint32_t rate = 0; rate < 128u; ++rate)
            for (uint32_t mode = 0; mode < 4u; ++mode) // bit 14 decrease, bit 15 exponential
                sustain.emplace_back(0x0008u, static_cast<uint16_t>((mode << 14) | (rate << 6)));
        for (uint32_t shift = 0; shift < 32u; ++shift)
            for (uint32_t mode = 0; mode < 2u; ++mode)
                release.emplace_back(0x0008u, static_cast<uint16_t>(0x1FC0u | (mode << 5) | shift));
        failed |= testAdsr(attack, 1u << 30, 30000u, "attack");
        failed |= testAdsr(decay, 1u << 30, 30000u, "decay");
        failed |= testAdsr(sustain, 1u << 30, 40000u, "sustain");
        failed |= testAdsr(release, 2000u, 30000u, "release");
        // DoD's own settings (DOD_IO_CENSUS: ADSR1 bits 0x80FF, ADSR2 bits 0x5FCB in use), key off midway.
        std::vector<std::pair<uint16_t, uint16_t>> dod;
        for (const uint16_t a1 : {0x00FFu, 0x80FFu, 0x000Fu, 0x80F0u, 0x0057u, 0x8023u})
            for (const uint16_t a2 : {0x1FC0u, 0x5FC0u, 0x4FCBu, 0x1A8Bu, 0x5F0Au, 0x0E01u})
                dod.emplace_back(a1, a2);
        failed |= testAdsr(dod, 6000u, 30000u, "DoD-like");
        failed |= testVolumeSlides(30000u);
        return failed;
    }

    // --ir <file> [evol]: the impulse response of ps2xIOP's core 1 reverb with DoD's room 0x386 settings (both
    // channels of the sound data input get one full-scale sample, wet only, EVOL as given, default 0x2800 as the game
    // sets it there), 6 s of core 1 output as raw 48 kHz stereo int16, for reverb_compare.py --ir.
    int writeImpulseResponse(const char *path, uint16_t evol)
    {
        IopSpu2 spu;
        for (const RegVal &r : kCoreRegs)
            spu.write16(0x400u + r.reg, r.value);
        for (const RegVal &r : kMasterRegs)
            spu.write16(r.reg + 0x28u, r.value);
        spu.write16(0x78Cu, evol);
        spu.write16(0x78Eu, evol);
        spu.write16(0x788u, 0x3FFF);
        spu.write16(0x78Au, 0x3FFF);
        spu.write16(0x794u, 0x7FFF);
        spu.write16(0x796u, 0x7FFF);
        spu.write16(0x598u, 0x0030); // MMIX: sound data input wet only
        spu.write16(0x59Au, 0xC081);
        std::FILE *f = std::fopen(path, "wb");
        if (!f)
            return 1;
        for (uint32_t n = 0; n < 6u * 48000u; ++n)
        {
            const int16_t v = n == 0u ? 0x7FFF : 0;
            for (const uint32_t area : {0x2400u, 0x2600u})
            {
                setTsa(spu, area + (n & 0x1FFu));
                spu.write16(0x400u + 0x1ACu, static_cast<uint16_t>(v));
            }
            int16_t out[2] = {0, 0};
            (void)spu.generate(out, 1u);
            std::fwrite(out, sizeof(out), 1, f);
        }
        std::fclose(f);
        std::printf("impulse response written to %s\n", path);
        return 0;
    }
}

int main(int argc, char **argv)
{
    if (argc > 2 && std::strcmp(argv[1], "--ir") == 0)
        return writeImpulseResponse(argv[2], argc > 3 ? static_cast<uint16_t>(std::strtoul(argv[3], nullptr, 16)) : 0x2800u);
    const uint32_t seconds = argc > 1 ? static_cast<uint32_t>(std::atoi(argv[1])) : 30u;
    int failed = testReverb(seconds);
    for (const uint16_t pitch : {0x1000u, 0x1A3Bu, 0x0C35u, 0x3FFFu, 0x0123u, 0x2D51u})
        failed |= testGauss(pitch, 48000u);
    failed |= testEnvelopes();
    std::printf(failed ? "DIFFERENCES FOUND\n" : "all equal\n");
    return failed;
}
