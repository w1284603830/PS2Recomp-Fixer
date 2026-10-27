// [DoD] Minimal stand-in for PCSX2's pcsx2/SPU2/defs.h, so that PCSX2's own SPU2/Reverb.cpp and
// SPU2/ReverbResample.cpp compile unchanged in the offline differential test (spu2_diff). Only what those two files
// use; V_Reverb and the V_Core reverb fields are copied from PCSX2's defs.h (same types, same names).
#pragma once

#include "common/Pcsx2Defs.h"

#include <algorithm>
#include <array>
#include <cstdint>

#define MULTI_ISA_UNSHARED_START
#define MULTI_ISA_UNSHARED_END

struct StereoOut32
{
	static const StereoOut32 Empty;

	s32 Left;
	s32 Right;

	StereoOut32() = default;

	StereoOut32(s32 left, s32 right)
		: Left(left)
		, Right(right)
	{
	}

	StereoOut32 operator+(const StereoOut32& right) const
	{
		return StereoOut32(Left + right.Left, Right + right.Right);
	}
};

static __forceinline s32 clamp_mix(s32 x)
{
	return std::clamp(x, -0x8000, 0x7fff);
}

static __forceinline StereoOut32 clamp_mix(StereoOut32 sample)
{
	return StereoOut32(clamp_mix(sample.Left), clamp_mix(sample.Right));
}

static __forceinline s16 SignExtend16(u16 v)
{
	return (s16)v;
}

// V_VolumeSlide and V_ADSR as in PCSX2's defs.h (for ADSR.cpp).
struct V_VolumeSlide
{
	union
	{
		u16 Reg_VOL;
		struct
		{
			u16 Step : 2;
			u16 Shift : 5;
			u16 : 5;
			u16 Phase : 1;
			u16 Decr : 1;
			u16 Exp : 1;
			u16 Enable : 1;
		};
	};

	u32 Counter;
	s32 Value;

public:
	V_VolumeSlide() = default;
	void Update();
	void RegSet(u16 src);
};

struct V_ADSR
{
	union
	{
		u32 reg32;

		struct
		{
			u16 regADSR1;
			u16 regADSR2;
		};

		struct
		{
			u32 SustainLevel : 4;
			u32 DecayShift : 4;
			u32 AttackStep : 2;
			u32 AttackShift : 5;
			u32 AttackMode : 1;
			u32 ReleaseShift : 5;
			u32 ReleaseMode : 1;
			u32 SustainStep : 2;
			u32 SustainShift : 5;
			u32 : 1;
			u32 SustainDir : 1;
			u32 SustainMode : 1;
		};
	};

	static constexpr int ADSR_PHASES = 5;

	static constexpr int PHASE_STOPPED = 0;
	static constexpr int PHASE_ATTACK = 1;
	static constexpr int PHASE_DECAY = 2;
	static constexpr int PHASE_SUSTAIN = 3;
	static constexpr int PHASE_RELEASE = 4;

	struct CachedADSR
	{
		bool Decr;
		bool Exp;
		u8 Shift;
		s8 Step;
		s32 Target;
	};

	std::array<CachedADSR, ADSR_PHASES> CachedPhases;

	u32 Counter;
	s32 Value;
	u8 Phase;

public:
	void UpdateCache();
	bool Calculate(int voiceidx);
	void Attack();
	void Release();
};

struct V_Reverb
{
	s16 IN_COEF_L;
	s16 IN_COEF_R;

	u32 APF1_SIZE;
	u32 APF2_SIZE;

	s16 APF1_VOL;
	s16 APF2_VOL;

	u32 SAME_L_SRC;
	u32 SAME_R_SRC;
	u32 DIFF_L_SRC;
	u32 DIFF_R_SRC;
	u32 SAME_L_DST;
	u32 SAME_R_DST;
	u32 DIFF_L_DST;
	u32 DIFF_R_DST;

	s16 IIR_VOL;
	s16 WALL_VOL;

	u32 COMB1_L_SRC;
	u32 COMB1_R_SRC;
	u32 COMB2_L_SRC;
	u32 COMB2_R_SRC;
	u32 COMB3_L_SRC;
	u32 COMB3_R_SRC;
	u32 COMB4_L_SRC;
	u32 COMB4_R_SRC;

	s16 COMB1_VOL;
	s16 COMB2_VOL;
	s16 COMB3_VOL;
	s16 COMB4_VOL;

	u32 APF1_L_DST;
	u32 APF1_R_DST;
	u32 APF2_L_DST;
	u32 APF2_R_DST;
};

struct V_Core
{
	int Index;
	bool IRQEnable;
	u32 IRQA;
	bool FxEnable;

	V_Reverb Revb;
	s16 RevbDownBuf[2][64 * 2];
	s16 RevbUpBuf[2][64 * 2];
	u32 RevbSampleBufPos;
	u32 EffectsStartA;
	u32 EffectsEndA;

	void AnalyzeReverbPreset();
	s32 RevbGetIndexer(s32 offset);
	StereoOut32 DoReverb(StereoOut32 Input);
};

extern V_Core Cores[2];
extern u32 Cycles;
extern s16 _spu2mem[0x200000 / sizeof(s16)];
void SetIrqCall(int core);

s32 ReverbDownsample(V_Core& core, bool right);
StereoOut32 ReverbUpsample(V_Core& core);
