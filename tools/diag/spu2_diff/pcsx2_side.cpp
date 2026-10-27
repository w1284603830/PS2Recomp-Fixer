// [DoD] spu2_diff, PCSX2 side: PCSX2's own SPU2 sources compiled unchanged (include path: shim, pcsx2, PCSX2 root).
// DIFF_PCSX2_REFERENCE: Reverb.cpp calls the reference (scalar) resampling filters; otherwise the SIMD ones PCSX2
// really runs (AVX2 with /arch:AVX2, else SSE4.1).
#include "SPU2/defs.h"
#include "SPU2/regs.h"
#include "SPU2/interpolate_table.h"

#include "SPU2/ReverbResample.cpp"
#ifdef DIFF_PCSX2_REFERENCE
#define ReverbDownsample ReverbDownsample_reference
#define ReverbUpsample ReverbUpsample_reference
#endif
#include "SPU2/Reverb.cpp"
#undef ReverbDownsample
#undef ReverbUpsample
#include "SPU2/ADSR.cpp"

#include "pcsx2_side.h"

#include <cstring>

const StereoOut32 StereoOut32::Empty(0, 0);
V_Core Cores[2];
u32 Cycles;
s16 _spu2mem[0x200000 / sizeof(s16)];
void SetIrqCall(int) {}
void pxOnAssertFail(const char*, int, const char*, const char*) {}

namespace pcsx2side
{
    void reset()
    {
        std::memset(_spu2mem, 0, sizeof(_spu2mem));
        for (int c = 0; c < 2; ++c)
        {
            V_Core& core = Cores[c];
            std::memset(&core, 0, sizeof(core));
            core.Index = c;
            // V_Core::Init (spu2sys.cpp)
            core.EffectsStartA = c ? 0xFFFF8 : 0xEFFF8;
            core.EffectsEndA = c ? 0xFFFFF : 0xEFFFF;
            core.FxEnable = false;
            core.IRQA = 0x800;
            core.IRQEnable = false;
        }
    }

    // RegTable.cpp: ESA and the PREVB_REG pairs are (high halfword, low halfword) of a u32; EEA only its high
    // halfword; the master area volumes are plain s16 (core 1 at +0x28).
    void writeReg(int c, uint32_t reg, uint16_t v)
    {
        V_Core& core = Cores[c];
        const auto pair = [&](uint32_t base, u32& field) {
            if (reg == base)
                field = (field & 0xFFFF) | (static_cast<u32>(v) << 16);
            else if (reg == base + 2)
                field = (field & 0xFFFF0000) | v;
        };
        V_Reverb& r = core.Revb;
        pair(REG_A_ESA, core.EffectsStartA);
        pair(R_APF1_SIZE, r.APF1_SIZE);
        pair(R_APF2_SIZE, r.APF2_SIZE);
        pair(R_SAME_L_DST, r.SAME_L_DST);
        pair(R_SAME_R_DST, r.SAME_R_DST);
        pair(R_COMB1_L_SRC, r.COMB1_L_SRC);
        pair(R_COMB1_R_SRC, r.COMB1_R_SRC);
        pair(R_COMB2_L_SRC, r.COMB2_L_SRC);
        pair(R_COMB2_R_SRC, r.COMB2_R_SRC);
        pair(R_SAME_L_SRC, r.SAME_L_SRC);
        pair(R_SAME_R_SRC, r.SAME_R_SRC);
        pair(R_DIFF_L_DST, r.DIFF_L_DST);
        pair(R_DIFF_R_DST, r.DIFF_R_DST);
        pair(R_COMB3_L_SRC, r.COMB3_L_SRC);
        pair(R_COMB3_R_SRC, r.COMB3_R_SRC);
        pair(R_COMB4_L_SRC, r.COMB4_L_SRC);
        pair(R_COMB4_R_SRC, r.COMB4_R_SRC);
        pair(R_DIFF_L_SRC, r.DIFF_L_SRC);
        pair(R_DIFF_R_SRC, r.DIFF_R_SRC);
        pair(R_APF1_L_DST, r.APF1_L_DST);
        pair(R_APF1_R_DST, r.APF1_R_DST);
        pair(R_APF2_L_DST, r.APF2_L_DST);
        pair(R_APF2_R_DST, r.APF2_R_DST);
        if (reg == REG_A_EEA)
            core.EffectsEndA = (core.EffectsEndA & 0xFFFF) | (static_cast<u32>(v) << 16);
        const s16 s = static_cast<s16>(v);
        switch (reg)
        {
            case R_IIR_VOL: r.IIR_VOL = s; break;
            case R_COMB1_VOL: r.COMB1_VOL = s; break;
            case R_COMB2_VOL: r.COMB2_VOL = s; break;
            case R_COMB3_VOL: r.COMB3_VOL = s; break;
            case R_COMB4_VOL: r.COMB4_VOL = s; break;
            case R_WALL_VOL: r.WALL_VOL = s; break;
            case R_APF1_VOL: r.APF1_VOL = s; break;
            case R_APF2_VOL: r.APF2_VOL = s; break;
            case R_IN_COEF_L: r.IN_COEF_L = s; break;
            case R_IN_COEF_R: r.IN_COEF_R = s; break;
            default: break;
        }
    }

    void setFxEnable(int c, bool on) { Cores[c].FxEnable = on; }

    void doReverb(int c, uint32_t cycles, int32_t inL, int32_t inR, int32_t& outL, int32_t& outR)
    {
        Cycles = cycles;
        const StereoOut32 out = Cores[c].DoReverb(StereoOut32(inL, inR));
        outL = out.Left;
        outR = out.Right;
    }

    int16_t* mem() { return _spu2mem; }

    int32_t voiceValue(uint32_t sp, const int16_t fifo[4])
    {
        int phase = (sp & 0x0ff0) >> 4;
        s32 out = 0;
        out += (interpTable[phase][0] * fifo[0]) >> 15;
        out += (interpTable[phase][1] * fifo[1]) >> 15;
        out += (interpTable[phase][2] * fifo[2]) >> 15;
        out += (interpTable[phase][3] * fifo[3]) >> 15;
        return out;
    }

    void adsrRun(uint16_t adsr1, uint16_t adsr2, uint32_t keyOffAt, uint32_t samples, int32_t* out)
    {
        V_ADSR a{};
        a.regADSR1 = adsr1;
        a.regADSR2 = adsr2;
        a.UpdateCache();
        a.Attack();
        for (uint32_t i = 0; i < samples; ++i)
        {
            if (i == keyOffAt)
                a.Release();
            if (a.Phase > V_ADSR::PHASE_STOPPED && !a.Calculate(0))
            {
                a.Value = 0; // V_Voice::Stop
                a.Phase = V_ADSR::PHASE_STOPPED;
            }
            out[i] = a.Value;
        }
    }

    void volumeRun(uint16_t start, uint16_t slide, uint32_t samples, int32_t* out)
    {
        V_VolumeSlide v{};
        v.RegSet(start);
        v.RegSet(slide);
        for (uint32_t i = 0; i < samples; ++i)
        {
            v.Update();
            out[i] = v.Value;
        }
    }

    const char* variant()
    {
#ifdef DIFF_PCSX2_REFERENCE
        return "PCSX2 reference resampling";
#elif _M_SSE >= 0x501
        return "PCSX2 AVX2 resampling";
#else
        return "PCSX2 SSE4.1 resampling";
#endif
    }
}
