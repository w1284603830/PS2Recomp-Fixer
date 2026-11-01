#pragma once
// Output-only, small-drift fractional delay filter (64-tap windowed sinc, 1024 phases), the host audio output's resampler. Does not advance guest time,
// change SPU2 mixing, or control the host queue. 48 kHz stereo PCM in/out.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace ps2audio {
class OutputResampler {
public:
    static constexpr unsigned Taps = 64;
    static constexpr unsigned Phases = 1024;
    // Relative to the previous linear interpolation of input[n-1]..input[n].
    static constexpr unsigned AddedDelay = Taps / 2 - 1;
    static constexpr double Cutoff = 0.94; // Nyquist fraction; safe for step <= 1.005.

    OutputResampler() { (void)table(); }

    template<class Emit>
    void push(int16_t left, int16_t right, double step, Emit&& emit) {
        // Bound the API to the same +/-0.5% drift as the existing host controller.
        step = std::isfinite(step) ? std::clamp(step, 0.995, 1.005) : 1.0;
        m_write = (m_write + 1u) & (Taps - 1u);
        m_history[m_write] = {left, right};
        while (m_phase < 1.0) {
            const double position = m_phase * Phases;
            const unsigned phase = std::min(static_cast<unsigned>(position), Phases - 1u);
            const double blend = position - phase;
            const auto& a = table()[phase];
            const auto& b = table()[phase + 1u];
            double l = 0.0, r = 0.0;
            for (unsigned k = 0; k < Taps; ++k) {
                const double coefficient = a[k] + (b[k] - a[k]) * blend;
                const auto& sample = m_history[(m_write - k) & (Taps - 1u)];
                l += sample[0] * coefficient;
                r += sample[1] * coefficient;
            }
            emit(pcm(l), pcm(r));
            m_phase += step;
        }
        m_phase -= 1.0;
    }

private:
    using Row = std::array<double, Taps>;
    using Table = std::array<Row, Phases + 1>;
    static const Table& table() {
        // Construct directly in static storage: a by-value lambda return can put a 512 KiB
        // temporary on the startup thread's stack when named return-value optimization is disabled.
        // Construct once, before opening the audio device; no heap or callback allocations.
        struct Coefficients {
            Table value{};
            Coefficients() {
                constexpr double pi = 3.1415926535897932384626433832795;
                constexpr double radius = Taps / 2.0;
                for (unsigned p = 0; p <= Phases; ++p) {
                    const double fraction = static_cast<double>(p) / Phases;
                    double sum = 0.0;
                    for (unsigned k = 0; k < Taps; ++k) {
                        const double x = k - radius + fraction;
                        const double sinc = std::abs(x) < 1e-12 ? Cutoff : std::sin(pi * Cutoff * x) / (pi * x);
                        const double window = 0.42 + 0.5 * std::cos(pi * x / radius) + 0.08 * std::cos(2.0 * pi * x / radius);
                        value[p][k] = sinc * window;
                        sum += value[p][k];
                    }
                    for (double& c : value[p]) c /= sum; // exact DC gain, no loudness enhancement.
                }
            }
        };
        static const Coefficients coefficients;
        return coefficients.value;
    }
    static int16_t pcm(double sample) {
        // A band-limited reconstruction can overshoot on full-scale transients.
        // Saturate, never wrap. No extra gain, compressor, or EQ is applied.
        return static_cast<int16_t>(std::clamp(std::lround(sample), -32768l, 32767l));
    }
    std::array<std::array<int16_t, 2>, Taps> m_history{};
    unsigned m_write = Taps - 1u;
    double m_phase = 0.0;
};
} // namespace ps2audio
