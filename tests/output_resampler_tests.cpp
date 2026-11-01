#ifndef OUTPUT_RESAMPLER_HEADER
#define OUTPUT_RESAMPLER_HEADER "../third_party/src/PS2Recomp/ps2xRuntime/src/lib/ps2_output_resampler.h"
#endif
#include OUTPUT_RESAMPLER_HEADER
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } } while (false)

int main(int argc, char **argv)
{
    using ps2audio::OutputResampler;
    CHECK(argc == 2);
    const std::string mode = argv[1];
    if (mode == "construct")
    {
        OutputResampler output;
        output.push(0, 0, 1.0, [](int16_t, int16_t) {});
    }
    else if (mode == "rates")
    {
        for (double step : {0.995, 1.0, 1.005})
        {
            OutputResampler output;
            unsigned count = 0;
            for (unsigned n = 0; n < 48000; ++n)
                output.push(0, 0, step, [&](int16_t, int16_t) { ++count; });
            CHECK(std::abs(count * step - 48000.0) <= step);
        }
    }
    else if (mode == "dc")
    {
        for (int16_t level : {int16_t(0), int16_t(12345), int16_t(-23456), int16_t(32767), int16_t(-32768)})
            for (double step : {0.995, 1.0, 1.005})
            {
                OutputResampler output;
                bool correct = true;
                for (unsigned n = 0; n < 512; ++n)
                    output.push(level, 0, step, [&](int16_t l, int16_t r) {
                        if (n > OutputResampler::Taps * 2)
                            correct &= l == level && r == 0;
                    });
                CHECK(correct);
            }
    }
    else if (mode == "delay")
    {
        OutputResampler output;
        std::vector<int16_t> left, right;
        for (unsigned n = 0; n < 160; ++n)
            output.push(n == 0 ? 20000 : 0, n == 16 ? -20000 : 0, 1.0,
                        [&](int16_t l, int16_t r) { left.push_back(l); right.push_back(r); });
        const auto peakL = std::max_element(left.begin(), left.end()) - left.begin();
        const auto peakR = std::min_element(right.begin(), right.end()) - right.begin();
        CHECK(peakL == OutputResampler::AddedDelay + 1);
        CHECK(peakR == peakL + 16);
        for (unsigned n = 96; n < left.size(); ++n)
            CHECK(left[n] == 0 && right[n] == 0);
    }
    else if (mode == "invalid-step")
    {
        const auto counts = [](double step) {
            OutputResampler output;
            unsigned count = 0;
            for (unsigned n = 0; n < 2000; ++n)
                output.push(1, -1, step, [&](int16_t, int16_t) { ++count; });
            return count;
        };
        CHECK(counts(-1.0) == counts(0.995));
        CHECK(counts(0.0) == counts(0.995));
        CHECK(counts(100.0) == counts(1.005));
        CHECK(counts(std::numeric_limits<double>::quiet_NaN()) == counts(1.0));
        CHECK(counts(std::numeric_limits<double>::infinity()) == counts(1.0));
    }
    else if (mode == "spectrum")
    {
        for (double frequency : {1000.0, 10000.0, 23900.0})
        {
            OutputResampler output;
            double energy = 0;
            unsigned count = 0;
            for (unsigned n = 0; n < 24000; ++n)
            {
                const auto value = static_cast<int16_t>(std::lround(12000.0 * std::sin(6.283185307179586 * frequency * n / 48000.0)));
                output.push(value, 0, 1.005, [&](int16_t l, int16_t) {
                    if (n >= 256) { energy += double(l) * l; ++count; }
                });
            }
            const double gain = std::sqrt(energy / count) / (12000.0 / std::sqrt(2.0));
            std::printf("%.0f Hz: gain %.8f\n", frequency, gain);
            // This 64-tap Blackman design must reject 23.9 kHz by at least 30 dB.
            CHECK(frequency > 23000.0 ? gain < std::pow(10.0, -30.0 / 20.0) : std::abs(gain - 1.0) < 0.005);
        }
    }
    else if (mode == "fingerprint")
    {
        uint64_t hash = 14695981039346656037ull;
        uint32_t state = 0x12345678u;
        OutputResampler output;
        for (unsigned n = 0; n < 12000; ++n)
        {
            const auto next = [&]() { state = state * 1664525u + 1013904223u; return static_cast<int16_t>(state >> 16); };
            const int16_t l = next(), r = next();
            const double step = n < 4000 ? 0.995 : n < 8000 ? 1.0 : 1.005;
            output.push(l, r, step, [&](int16_t left, int16_t right) {
                for (int16_t sample : {left, right}) {
                    const uint16_t bits = static_cast<uint16_t>(sample);
                    hash = (hash ^ (bits & 255u)) * 1099511628211ull;
                    hash = (hash ^ (bits >> 8)) * 1099511628211ull;
                }
            });
        }
        std::printf("%016llx\n", static_cast<unsigned long long>(hash));
    }
    else
        CHECK(false);
    return 0;
}
