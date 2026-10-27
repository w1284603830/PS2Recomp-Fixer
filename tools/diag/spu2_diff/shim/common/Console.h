// [DoD] Stand-in for PCSX2's common/Console.h in the spu2_diff test (Reverb.cpp's AnalyzeReverbPreset prints with it).
#pragma once

struct DodConsoleShim
{
	void WriteLn(const char*, ...) const {}
};
inline DodConsoleShim Console;
