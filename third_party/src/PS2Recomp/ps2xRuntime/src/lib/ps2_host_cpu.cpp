// [Fixer] Host CPU capability report (ps2_host_cpu.h). Plain CPUID/XGETBV only: this file must not contain any
// instruction beyond the x64 baseline, because it runs before the SSE4.1 check passes.
#include "ps2_host_cpu.h"

#include <cstdio>
#include <cstring>
#include <thread>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

namespace
{
    void cpuid(int out[4], int leaf, int subleaf = 0)
    {
#if defined(_MSC_VER)
        __cpuidex(out, leaf, subleaf);
#elif defined(__x86_64__) || defined(__i386__)
        unsigned a = 0, b = 0, c = 0, d = 0;
        __cpuid_count(leaf, subleaf, a, b, c, d);
        out[0] = static_cast<int>(a);
        out[1] = static_cast<int>(b);
        out[2] = static_cast<int>(c);
        out[3] = static_cast<int>(d);
#else
        out[0] = out[1] = out[2] = out[3] = 0;
#endif
    }

    unsigned long long xgetbv0()
    {
#if defined(_MSC_VER)
        return _xgetbv(0);
#elif defined(__x86_64__) || defined(__i386__)
        unsigned eax = 0, edx = 0;
        __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
        return (static_cast<unsigned long long>(edx) << 32) | eax;
#else
        return 0;
#endif
    }
}

namespace ps2host
{
    CpuReport cpuReport()
    {
        CpuReport r;
        r.logicalCores = std::thread::hardware_concurrency();
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
        int regs[4] = {};
        cpuid(regs, 0);
        const int maxLeaf = regs[0];
        char vendor[13] = {};
        std::memcpy(vendor + 0, &regs[1], 4);
        std::memcpy(vendor + 4, &regs[3], 4);
        std::memcpy(vendor + 8, &regs[2], 4);
        r.vendor = vendor;
        if (maxLeaf >= 1)
        {
            cpuid(regs, 1);
            const unsigned ecx = static_cast<unsigned>(regs[2]);
            r.sse41 = (ecx >> 19) & 1u;
            r.sse42 = (ecx >> 20) & 1u;
            r.popcnt = (ecx >> 23) & 1u;
            const bool fmaBit = (ecx >> 12) & 1u;
            const bool osxsave = (ecx >> 27) & 1u;
            const bool avxBit = (ecx >> 28) & 1u;
            r.osAvx = avxBit && osxsave && (xgetbv0() & 6ull) == 6ull;
            r.fma = r.osAvx && fmaBit;
            if (maxLeaf >= 7)
            {
                cpuid(regs, 7, 0);
                const unsigned ebx = static_cast<unsigned>(regs[1]);
                r.avx2 = r.osAvx && ((ebx >> 5) & 1u);
                r.bmi2 = (ebx >> 8) & 1u;
            }
        }
        cpuid(regs, static_cast<int>(0x80000000u));
        if (static_cast<unsigned>(regs[0]) >= 0x80000004u)
        {
            char brand[49] = {};
            for (int i = 0; i < 3; ++i)
            {
                cpuid(regs, static_cast<int>(0x80000002u + i));
                std::memcpy(brand + 16 * i, regs, 16);
            }
            const char *start = brand;
            while (*start == ' ')
                ++start;
            r.brand = start;
        }
#endif
        return r;
    }

    bool cpuMeetsBaseline(std::string *missing)
    {
        const CpuReport r = cpuReport();
        std::fprintf(stderr, "[cpu] %s | %s | %u logical | SSE4.1 %s SSE4.2 %s AVX(OS) %s AVX2 %s FMA %s BMI2 %s\n",
                     r.brand.empty() ? "?" : r.brand.c_str(), r.vendor.c_str(), r.logicalCores,
                     r.sse41 ? "yes" : "NO", r.sse42 ? "yes" : "no", r.osAvx ? "yes" : "no", r.avx2 ? "yes" : "no",
                     r.fma ? "yes" : "no", r.bmi2 ? "yes" : "no");
        if (r.sse41)
            return true;
        if (missing)
            *missing = "CPU lacks SSE4.1 (" + (r.brand.empty() ? r.vendor : r.brand) + ")";
        return false;
    }
}
