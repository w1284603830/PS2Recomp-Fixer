// Test the production private FIR without adding a public runtime testing API.
#include "../third_party/src/PS2Recomp/ps2xIOP/src/emulator/core/iop_spu2.cpp"
#include <tmmintrin.h>

// Same instruction sequence as pinned PCSX2 646df006a4, SPU2/ReverbResample.cpp SSE path:
// PMULHRSW per tap, saturating lane accumulation, then three PHADDSW reductions.
__attribute__((target("ssse3")))
int16_t reference(const std::array<int16_t, 128> &buffer, unsigned pos, const std::array<int16_t, 39> &coefs)
{
    std::array<int16_t, 48> padded{};
    std::copy(coefs.begin(), coefs.end(), padded.begin());
    const unsigned first = (pos - 39u) & 63u;
    __m128i sum = _mm_setzero_si128();
    for (unsigned i = 0; i < 40; i += 8)
    {
        const __m128i samples = _mm_loadu_si128(reinterpret_cast<const __m128i *>(&buffer[first + i]));
        const __m128i coefficients = _mm_loadu_si128(reinterpret_cast<const __m128i *>(&padded[i]));
        sum = _mm_adds_epi16(sum, _mm_mulhrs_epi16(samples, coefficients));
    }
    sum = _mm_hadds_epi16(sum, sum);
    sum = _mm_hadds_epi16(sum, sum);
    sum = _mm_hadds_epi16(sum, sum);
    return static_cast<int16_t>(_mm_extract_epi16(sum, 0));
}

int main()
{
    if (!__builtin_cpu_supports("ssse3")) return 77;
    using namespace ps2x::iop::detail;
    uint32_t state = 0x31415926u;
    const auto next = [&]() { state = state * 1664525u + 1013904223u; return static_cast<int16_t>(state >> 16); };
    for (unsigned trial = 0; trial < 20000; ++trial)
    {
        std::array<int16_t, 128> buffer{};
        for (unsigned n = 0; n < 64; ++n)
            buffer[n] = buffer[n + 64] = trial == 0 ? 32767 : trial == 1 ? -32768 : next();
        for (unsigned pos = 0; pos < 64; ++pos)
            for (const auto &coefs : {kRevbDownCoefs, kRevbUpCoefs})
                if (reverbFir(buffer, pos, coefs) != reference(buffer, pos, coefs))
                {
                    std::fprintf(stderr, "FIR mismatch: trial %u, ring position %u\n", trial, pos);
                    return 1;
                }
    }
    std::puts("2560000 FIR comparisons match the pinned PCSX2 SSE instruction sequence");
}
