#ifndef PS2_HOST_CPU_H
#define PS2_HOST_CPU_H

// [Fixer] Host CPU capability report, read before anything ISA-specific runs.
//
// The runtime, the generated code and pcsx2gs.dll use SSE4.1 (blendv, pmaxsd, PCSX2's baseline). AVX/AVX2 are only
// used behind run-time checks (microVU JIT, CRT, FFmpeg). A host without SSE4.1 cannot run the port at all; checking
// first turns the illegal-instruction crash into a readable message. The report also names the host for user logs
// ("old Xeon cannot start" reports): brand, logical cores and which vector ISA the OS actually enables (AVX needs
// OSXSAVE + XCR0 YMM state, not only the CPUID bit).

#include <string>

namespace ps2host
{
    struct CpuReport
    {
        std::string vendor;
        std::string brand;
        unsigned logicalCores = 0;
        bool sse41 = false;
        bool sse42 = false;
        bool popcnt = false;
        bool osAvx = false; // CPUID AVX + OSXSAVE + XCR0[2:1] == 11b
        bool avx2 = false;  // only meaningful with osAvx
        bool fma = false;   // only meaningful with osAvx
        bool bmi2 = false;
    };

    CpuReport cpuReport();

    // Logs one "[cpu] ..." line to stderr and returns whether the host meets the runtime's baseline (SSE4.1).
    // `missing` receives a short user-facing reason when it does not.
    bool cpuMeetsBaseline(std::string *missing = nullptr);
}

#endif
