// [DoD] spu2_diff: PCSX2's SPU2 reverb (its own Reverb.cpp / ReverbResample.cpp, see pcsx2_side.cpp) behind a small
// interface without PCSX2 types, and PCSX2's voice interpolation formula with its own table.
#pragma once

#include <cstdint>

namespace pcsx2side
{
    void reset();
    // A reverb register write as PCSX2 maps it (pcsx2/SPU2/regs.h names): `reg` is the core 0 offset (0x2E0..0x33E
    // core area, 0x774..0x786 master area volumes).
    void writeReg(int core, uint32_t reg, uint16_t value);
    void setFxEnable(int core, bool on);
    // V_Core::DoReverb with Cycles = `cycles`.
    void doReverb(int core, uint32_t cycles, int32_t inL, int32_t inR, int32_t &outL, int32_t &outR);
    int16_t *mem();
    // Mixer.cpp GetVoiceValues: interpTable[phase][k] * fifo[k] >> 15 summed, phase = (sp & 0x0ff0) >> 4.
    int32_t voiceValue(uint32_t sp, const int16_t fifo[4]);
    // ADSR.cpp: the envelope of a voice keyed on at sample 0 (Attack) and off at keyOffAt (Release), its Value after
    // each sample's Calculate (0 once V_Voice::Stop).
    void adsrRun(uint16_t adsr1, uint16_t adsr2, uint32_t keyOffAt, uint32_t samples, int32_t *out);
    // ADSR.cpp: V_VolumeSlide set to `start` (a fixed volume), then to `slide`, its Value after each Update.
    void volumeRun(uint16_t start, uint16_t slide, uint32_t samples, int32_t *out);
    const char *variant();
}
