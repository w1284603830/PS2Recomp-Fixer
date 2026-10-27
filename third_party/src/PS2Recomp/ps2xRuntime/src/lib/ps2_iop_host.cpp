#include "ps2_iop_host.h"
#include "ps2_iop_post.h"
#include "ps2x/iop/iop_subsystem.h"

#include "ps2_runtime.h"
#include "ps2_stubs.h"
#include "Kernel/Stubs/SIF.h"
#include "runtime/ps2_memory.h"
#include "Kernel/Stubs/MemoryCard.h"
#include "Kernel/Syscalls/Common.h"
#include "ps2_host_backend.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <utility>

#if !defined(_WIN32)
#include <sys/types.h>
#endif

PS2IopHostAdapter::CallScope::CallScope(PS2IopHostAdapter &owner, R5900Context *context, uint8_t *rdram)
    : m_lock(owner.m_callMutex),
      m_owner(&owner),
      m_previousContext(owner.m_activeContext),
      m_previousRdram(owner.m_activeRdram),
      m_previousToken(owner.m_activeToken)
{
    m_token = owner.m_nextToken++;
    if (m_token == 0)
    {
        m_token = owner.m_nextToken++;
    }
    owner.m_activeContext = context;
    owner.m_activeRdram = rdram;
    owner.m_activeToken = m_token;
}

PS2IopHostAdapter::CallScope::~CallScope()
{
    release();
}

PS2IopHostAdapter::CallScope::CallScope(CallScope &&other) noexcept
    : m_lock(std::move(other.m_lock)),
      m_owner(std::exchange(other.m_owner, nullptr)),
      m_previousContext(other.m_previousContext),
      m_previousRdram(other.m_previousRdram),
      m_previousToken(other.m_previousToken),
      m_token(other.m_token)
{
}

PS2IopHostAdapter::CallScope &PS2IopHostAdapter::CallScope::operator=(CallScope &&other) noexcept
{
    if (this != &other)
    {
        release();
        m_lock = std::move(other.m_lock);
        m_owner = std::exchange(other.m_owner, nullptr);
        m_previousContext = other.m_previousContext;
        m_previousRdram = other.m_previousRdram;
        m_previousToken = other.m_previousToken;
        m_token = other.m_token;
    }
    return *this;
}

void PS2IopHostAdapter::CallScope::release()
{
    if (m_owner)
    {
        m_owner->m_activeContext = m_previousContext;
        m_owner->m_activeRdram = m_previousRdram;
        m_owner->m_activeToken = m_previousToken;
        m_owner = nullptr;
    }
}

namespace ps2x::iop
{
    extern void (*g_dodSpu2StemsSink)(const int16_t *stems6); // ps2xIOP iop_spu2.cpp
    extern void (*g_dodSpu2EventSink)(const char *text);
}

// [DoD] DOD_STALL_WATCH=1 (diagnostics): when the SPU2 output last got sound from the IOP thread and when the sound
// card's callback last ran (steady-clock ns), read by the runner's stall watcher (runner/main.cpp).
std::atomic<uint64_t> g_dodSpu2PushNs{0};
std::atomic<uint64_t> g_dodAudioCallbackNs{0};

namespace
{
    bool dodStallWatchHost()
    {
        // As ps2xIOP's dodStallWatchOn: DOD_STALL_WATCH=1 or the audio trace (DOD_AUDIO_TRACE); DOD_STALL_WATCH=0 = off.
        static const bool on = []() {
            const char *v = std::getenv("DOD_STALL_WATCH");
            if (v && *v)
                return *v != '0';
            const char *trace = std::getenv("DOD_AUDIO_TRACE");
            return trace != nullptr && *trace != '\0';
        }();
        return on;
    }

    uint64_t dodSteadyNs()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
    }
}

namespace
{
    // [DoD] Audio trace for the test launcher (DOD_AUDIO_TRACE=<folder>). The last 60 s of what the sound card got, of
    // the SPU2 mix and of its stems stay in memory, with an event log: output underruns and drops, AutoDMA underruns
    // (from the SPU2), late sound card callbacks, pauses of the SPU2 output and one status line a second. F9 in the
    // game window (dodAudioTraceSave) writes them to <folder>\<date-time>\ as WAV files plus events.txt, so a pop the
    // tester has just heard is kept without recording all the time. Off: nothing is kept and the hooks stay null.
    class DodAudioTrace
    {
    public:
        static constexpr uint32_t kRate = 48000u;
        static constexpr size_t kSeconds = 60u;

        DodAudioTrace()
        {
            const char *dir = std::getenv("DOD_AUDIO_TRACE");
            if (!dir || !*dir)
                return;
            m_dir = dir;
            m_mix.init(2u);
            m_out.init(2u);
            m_stems.init(6u);
            m_start = std::chrono::steady_clock::now();
            m_on = true;
        }

        bool on() const { return m_on; }

        double now() const
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count();
        }

        // IOP thread: the SPU2 mix as generated; a pause between two blocks over 60 ms means the IOP fell behind.
        void mix(const int16_t *samples, size_t frames)
        {
            const double t = now();
            if (m_lastMix > 0.0 && t - m_lastMix > 0.060)
                event("SPU2 produced no sound for %.0f ms (the IOP thread fell behind)", (t - m_lastMix) * 1000.0);
            m_lastMix = t;
            m_mix.push(samples, frames);
        }

        void out(const int16_t *samples, size_t frames) { m_out.push(samples, frames); }
        void stems(const int16_t *stems6) { m_stems.push(stems6, 1u); }

        void event(const char *format, ...)
        {
            if (!m_on)
                return;
            char text[256];
            va_list args;
            va_start(args, format);
            std::vsnprintf(text, sizeof(text), format, args);
            va_end(args);
            const double t = now();
            static const bool echo = std::getenv("DOD_AUDIO_TRACE_ECHO") != nullptr; // also on stderr (tests)
            if (echo && std::strncmp(text, "status:", 7) != 0)
                std::fprintf(stderr, "[audio-trace] %.3f s %s\n", t, text);
            std::lock_guard<std::mutex> lock(m_eventMutex);
            m_events.emplace_back(t, text);
            while (!m_events.empty() && (m_events.front().first < t - static_cast<double>(kSeconds) || m_events.size() > 20000u))
                m_events.pop_front();
        }

        // Window thread (F9). Copies the rings and writes the files on a worker thread; returns the folder name.
        std::string save()
        {
            if (!m_on)
                return {};
            const std::time_t wall = std::time(nullptr);
            std::tm local{};
            localtime_s(&local, &wall);
            char name[32];
            std::strftime(name, sizeof(name), "%Y%m%d-%H%M%S", &local);
            std::filesystem::path dir = std::filesystem::path(m_dir) / name;
            for (int n = 2; std::filesystem::exists(dir); ++n)
                dir = std::filesystem::path(m_dir) / (std::string(name) + "-" + std::to_string(n));
            const double at = now();
            event("F9: trace saved to %s", dir.filename().string().c_str());
            std::vector<int16_t> mix = m_mix.snapshot(), out = m_out.snapshot(), stems = m_stems.snapshot();
            std::vector<std::pair<double, std::string>> events;
            {
                std::lock_guard<std::mutex> lock(m_eventMutex);
                events.assign(m_events.begin(), m_events.end());
            }
            const std::string cdImage = PS2Runtime::getIoPaths().cdImage.string();
            std::thread([dir, at, mix = std::move(mix), out = std::move(out), stems = std::move(stems),
                         events = std::move(events), cdImage]()
                        {
                            std::error_code error;
                            std::filesystem::create_directories(dir, error);
                            writeWav(dir / "output.wav", out, 2u, 0u);
                            writeWav(dir / "spu2_mix.wav", mix, 2u, 0u);
                            writeWav(dir / "stem_core0.wav", stems, 6u, 0u);
                            writeWav(dir / "stem_stream_input.wav", stems, 6u, 2u);
                            writeWav(dir / "stem_voices.wav", stems, 6u, 4u);
                            if (std::FILE *f = std::fopen((dir / "events.txt").string().c_str(), "w"))
                            {
                                for (const auto &[t, text] : events)
                                    std::fprintf(f, "%10.3f s  %s\n", t, text.c_str());
                                std::fclose(f);
                            }
                            if (std::FILE *f = std::fopen((dir / "info.txt").string().c_str(), "w"))
                            {
                                std::fprintf(f, "Dawn of Dreams audio trace, saved at game time %.3f s (F9).\n", at);
                                std::fprintf(f, "Disc image: %s\n\n", cdImage.c_str());
                                std::fprintf(f, "output.wav             what the sound card played (after resampling and fades); ends at F9 (%.1f s)\n",
                                             out.size() / 2.0 / kRate);
                                std::fprintf(f, "spu2_mix.wav           the SPU2 mix as the game produced it; ends about 0.1 s (the output queue) after output.wav\n");
                                std::fprintf(f, "stem_core0.wav         SPU2 core 0 output (sound effects)\n");
                                std::fprintf(f, "stem_stream_input.wav  SPU2 core 1 sound data input (the ADX stream: music, voices, movies)\n");
                                std::fprintf(f, "stem_voices.wav        SPU2 core 1 voices\n");
                                std::fprintf(f, "events.txt             output underruns / drops, AutoDMA underruns, late sound card callbacks,\n"
                                                "                       SPU2 pauses and a status line a second (game time in seconds)\n");
                                std::fclose(f);
                            }
                            std::fprintf(stderr, "[audio-trace] saved %s\n", dir.string().c_str());
                        })
                .detach();
            return dir.filename().string();
        }

    private:
        struct Ring
        {
            std::mutex mutex;
            std::vector<int16_t> data;
            size_t channels = 2u;
            size_t pos = 0u;    // next frame to write
            size_t filled = 0u; // frames held (up to the capacity)

            void init(size_t ch)
            {
                channels = ch;
                data.assign(kRate * kSeconds * ch, 0);
            }

            void push(const int16_t *samples, size_t frames)
            {
                std::lock_guard<std::mutex> lock(mutex);
                const size_t capacity = data.size() / channels;
                for (size_t i = 0; i < frames;)
                {
                    const size_t run = std::min(frames - i, capacity - pos);
                    std::memcpy(&data[pos * channels], samples + i * channels, run * channels * sizeof(int16_t));
                    pos = (pos + run) % capacity;
                    filled = std::min(capacity, filled + run);
                    i += run;
                }
            }

            std::vector<int16_t> snapshot()
            {
                std::lock_guard<std::mutex> lock(mutex);
                const size_t capacity = data.size() / channels;
                std::vector<int16_t> out(filled * channels);
                const size_t first = (pos + capacity - filled) % capacity;
                for (size_t i = 0; i < filled; ++i)
                    std::memcpy(&out[i * channels], &data[((first + i) % capacity) * channels], channels * sizeof(int16_t));
                return out;
            }
        };

        // Stereo 48 kHz 16-bit WAV of channels [first, first + 1] of an interleaved buffer.
        static void writeWav(const std::filesystem::path &path, const std::vector<int16_t> &data, size_t channels, size_t first)
        {
            std::FILE *f = std::fopen(path.string().c_str(), "wb");
            if (!f)
                return;
            const uint32_t frames = static_cast<uint32_t>(data.size() / channels);
            const uint32_t bytes = frames * 4u;
            const uint32_t riff = bytes + 36u;
            const uint8_t header[44] = {'R', 'I', 'F', 'F', static_cast<uint8_t>(riff), static_cast<uint8_t>(riff >> 8),
                                        static_cast<uint8_t>(riff >> 16), static_cast<uint8_t>(riff >> 24), 'W', 'A', 'V', 'E',
                                        'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0, 0x80, 0xBB, 0, 0, 0x00, 0xEE, 2, 0, 4, 0, 16, 0,
                                        'd', 'a', 't', 'a', static_cast<uint8_t>(bytes), static_cast<uint8_t>(bytes >> 8),
                                        static_cast<uint8_t>(bytes >> 16), static_cast<uint8_t>(bytes >> 24)};
            std::fwrite(header, 1, sizeof(header), f);
            std::vector<int16_t> pair(static_cast<size_t>(frames) * 2u);
            for (size_t i = 0; i < frames; ++i)
            {
                pair[i * 2u] = data[i * channels + first];
                pair[i * 2u + 1u] = data[i * channels + first + 1u];
            }
            std::fwrite(pair.data(), sizeof(int16_t), pair.size(), f);
            std::fclose(f);
        }

        bool m_on = false;
        std::string m_dir;
        std::chrono::steady_clock::time_point m_start{};
        double m_lastMix = 0.0;
        Ring m_mix, m_out, m_stems;
        std::mutex m_eventMutex;
        std::deque<std::pair<double, std::string>> m_events;
    };

    DodAudioTrace g_dodAudioTrace;

    // The SPU2's hooks (ps2xIOP) are null unless the trace is on.
    const bool g_dodAudioTraceHooks = []()
    {
        if (g_dodAudioTrace.on())
        {
            ps2x::iop::g_dodSpu2StemsSink = [](const int16_t *stems6) { g_dodAudioTrace.stems(stems6); };
            ps2x::iop::g_dodSpu2EventSink = [](const char *text) { g_dodAudioTrace.event("%s", text); };
            std::fprintf(stderr, "[audio-trace] on: the last %zu s are kept; F9 saves them\n", DodAudioTrace::kSeconds);
        }
        return true;
    }();

    // [DoD] Host output of the emulated SPU2 (ran-j/PS2Recomp#308 plays it through PS2AudioBackend; kept here so the
    // runtime header every generated file includes does not change). The IOP thread queues 48 kHz stereo; raylib's
    // audio thread plays it from a ring. Playback starts with 100 ms queued, which absorbs a stall of the game (a
    // shader compile, a slow frame) up to that long; an underrun fades the last sample out over 5 ms instead of
    // cutting to zero (the click), waits for 100 ms again and fades back in. A backlog over 300 ms drops the oldest
    // audio down to 150 ms.
    // DOD_SPU2_WAV=<file> also records the output; DOD_SPU2_STATS=1 prints level, queue and glitch counts every 5 s.
    class DodSpu2Output
    {
    public:
        static constexpr uint32_t kRate = 48000u;
        static constexpr size_t kCapacity = kRate;
        static constexpr size_t kPrimeFrames = kRate / 10u;
        static constexpr size_t kMaxFrames = kRate * 3u / 10u;
        static constexpr size_t kTargetFrames = kRate * 3u / 20u;
        static constexpr uint32_t kFadeFrames = kRate / 200u; // 5 ms

        void push(const int16_t *samples, size_t frames)
        {
            if (!samples || frames == 0u)
                return;
            if (dodStallWatchHost())
                g_dodSpu2PushNs.store(dodSteadyNs(), std::memory_order_relaxed);
            record(samples, frames);
            stats(samples, frames);
            if (g_dodAudioTrace.on())
                g_dodAudioTrace.mix(samples, frames);
            if (!m_opened)
            {
                // Open the host stream on the first audible output.
                if (std::all_of(samples, samples + frames * 2u, [](int16_t v) { return v == 0; }))
                    return;
                if (!IsAudioDeviceReady())
                    return;
                m_opened = true;
                SetAudioStreamBufferSizeDefault(1024);
                m_stream = LoadAudioStream(kRate, 16, 2);
                s_active.store(this);
                SetAudioStreamCallback(m_stream, &DodSpu2Output::callback);
                PlayAudioStream(m_stream);
                std::fprintf(stderr, "[spu2] host audio stream opened (48 kHz stereo)\n");
            }
            std::lock_guard<std::mutex> lock(m_mutex);
            // Drift: the game's clock and the sound card's never run at exactly the same rate (with the vsync lock the
            // game runs 0.1 % fast). The output is resampled by linear interpolation at a rate that follows the queue
            // (+-0.5 % at most, 0.2 % for 20 ms off the 100 ms target), so the waveform stays continuous. Skipping one
            // frame in 256 instead, as before, put a tiny click in sustained music about a minute in (user report).
            double step = 1.0;
            if (m_primed)
            {
                const double off = (static_cast<double>(m_queued) - static_cast<double>(kPrimeFrames)) / kRate;
                step = 1.0 + std::clamp(off * 0.1, -0.005, 0.005);
            }
            for (size_t i = 0; i < frames; ++i)
            {
                const int32_t left = samples[i * 2u];
                const int32_t right = samples[i * 2u + 1u];
                while (m_resamplePos < 1.0)
                {
                    const double t = m_resamplePos;
                    pushFrame(static_cast<int16_t>(std::lround(m_prevLeft + (left - m_prevLeft) * t)),
                              static_cast<int16_t>(std::lround(m_prevRight + (right - m_prevRight) * t)));
                    m_resamplePos += step;
                }
                m_resamplePos -= 1.0;
                m_prevLeft = left;
                m_prevRight = right;
            }
            if (m_queued > kMaxFrames)
            {
                const size_t drop = m_queued - kTargetFrames;
                m_read = (m_read + drop) % kCapacity;
                m_queued -= drop;
                ++m_drops;
                g_dodAudioTrace.event("output queue over 300 ms: skipped %zu ms of older sound (a jump)", drop * 1000u / kRate);
            }
        }

        void pushFrame(int16_t left, int16_t right)
        {
            recordOut(left, right);
            {
                if (m_queued >= kCapacity)
                {
                    m_read = (m_read + 1u) % kCapacity;
                    --m_queued;
                }
                const size_t index = ((m_read + m_queued) % kCapacity) * 2u;
                m_ring[index] = left;
                m_ring[index + 1u] = right;
                ++m_queued;
            }
        }

        void close()
        {
            s_active.store(nullptr);
            if (m_opened && IsAudioDeviceReady())
            {
                StopAudioStream(m_stream);
                UnloadAudioStream(m_stream);
            }
            m_opened = false;
            finishRecording();
        }

    private:
        static void callback(void *buffer, unsigned int frames)
        {
            if (dodStallWatchHost())
                g_dodAudioCallbackNs.store(dodSteadyNs(), std::memory_order_relaxed);
            int16_t *out = static_cast<int16_t *>(buffer);
            DodSpu2Output *self = s_active.load();
            if (!self)
            {
                std::memset(out, 0, static_cast<size_t>(frames) * 4u);
                return;
            }
            {
                std::lock_guard<std::mutex> lock(self->m_mutex);
                self->fill(out, frames);
            }
            if (g_dodAudioTrace.on())
                self->traceCallback(out, frames);
            // DOD_MUTE=1 (long automated tests): the sound card gets silence; the SPU2, the queue and the trace run as usual.
            static const bool mute = std::getenv("DOD_MUTE") != nullptr;
            if (mute)
                std::memset(out, 0, static_cast<size_t>(frames) * 4u);
        }

        void fill(int16_t *out, unsigned int frames)
        {
            if (!m_primed && m_queued < kPrimeFrames)
            {
                fadeOut(out, frames);
                return;
            }
            if (!m_primed)
                m_fadeIn = 0u; // resuming: fade in from silence
            m_primed = true;
            const size_t available = std::min<size_t>(frames, m_queued);
            for (size_t i = 0; i < available; ++i)
            {
                const size_t index = ((m_read + i) % kCapacity) * 2u;
                int32_t left = m_ring[index];
                int32_t right = m_ring[index + 1u];
                if (m_fadeIn < kFadeFrames)
                {
                    left = left * static_cast<int32_t>(m_fadeIn) / static_cast<int32_t>(kFadeFrames);
                    right = right * static_cast<int32_t>(m_fadeIn) / static_cast<int32_t>(kFadeFrames);
                    ++m_fadeIn;
                }
                out[i * 2u] = static_cast<int16_t>(left);
                out[i * 2u + 1u] = static_cast<int16_t>(right);
                m_lastLeft = left;
                m_lastRight = right;
            }
            if (available < frames)
            {
                fadeOut(out + available * 2u, frames - static_cast<unsigned int>(available));
                m_primed = false;
                ++m_underruns;
                g_dodAudioTrace.event("output underrun: the sound card wanted %u frames, %zu were queued (fade out, refill 100 ms)",
                                      frames, available);
            }
            m_read = (m_read + available) % kCapacity;
            m_queued -= available;
        }

        // Audio trace (sound card thread): what was played, callbacks that came late, a status line a second.
        void traceCallback(const int16_t *out, unsigned int frames)
        {
            g_dodAudioTrace.out(out, frames);
            const double t = g_dodAudioTrace.now();
            if (m_traceLastCallback > 0.0)
            {
                const double gap = t - m_traceLastCallback;
                const double expected = static_cast<double>(m_traceLastFrames) / kRate;
                m_traceMaxGap = std::max(m_traceMaxGap, gap);
                if (gap > expected * 2.0 + 0.020)
                    g_dodAudioTrace.event("sound card callback late: %.1f ms since the previous one (%.1f ms of sound each)",
                                          gap * 1000.0, expected * 1000.0);
            }
            m_traceLastCallback = t;
            m_traceLastFrames = frames;
            ++m_traceCallbacks;
            if (t - m_traceStatusAt >= 1.0)
            {
                size_t queued;
                uint32_t underruns, drops;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    queued = m_queued;
                    underruns = m_underruns;
                    drops = m_drops;
                }
                g_dodAudioTrace.event("status: queue %zu ms, %u callbacks (longest gap %.1f ms), output underruns %u, drops %u",
                                      queued * 1000u / kRate, m_traceCallbacks, m_traceMaxGap * 1000.0, underruns, drops);
                m_traceStatusAt = t;
                m_traceCallbacks = 0u;
                m_traceMaxGap = 0.0;
            }
        }

        // Silence that starts from the last sample played and decays to (near) zero within about 5 ms (x 47/48 a sample).
        void fadeOut(int16_t *out, unsigned int frames)
        {
            for (unsigned int i = 0; i < frames; ++i)
            {
                const int32_t left = m_lastLeft * 47 / 48;
                const int32_t right = m_lastRight * 47 / 48;
                m_lastLeft = left;
                m_lastRight = right;
                out[i * 2u] = static_cast<int16_t>(left);
                out[i * 2u + 1u] = static_cast<int16_t>(right);
            }
        }

        // DOD_SPU2_OUT_WAV=<file>: raw 48 kHz stereo 16-bit of what goes into the host queue (after resampling).
        void recordOut(int16_t left, int16_t right)
        {
            static std::FILE *const file = []() -> std::FILE * {
                const char *path = std::getenv("DOD_SPU2_OUT_WAV");
                return path && *path ? std::fopen(path, "wb") : nullptr;
            }();
            if (file)
            {
                const int16_t frame[2] = {left, right};
                std::fwrite(frame, sizeof(frame), 1, file);
            }
        }

        void record(const int16_t *samples, size_t frames)
        {
            static const char *const path = std::getenv("DOD_SPU2_WAV");
            if (!path || !*path)
                return;
            if (!m_wav && !m_wavFailed)
            {
                m_wav = std::fopen(path, "wb");
                m_wavFailed = m_wav == nullptr;
                if (m_wav)
                {
                    const uint8_t header[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
                                                16, 0, 0, 0, 1, 0, 2, 0, 0x80, 0xBB, 0, 0, 0x00, 0xEE, 2, 0,
                                                4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
                    std::fwrite(header, 1, sizeof(header), m_wav);
                }
            }
            if (m_wav)
            {
                std::fwrite(samples, 4, frames, m_wav);
                m_wavBytes += static_cast<uint32_t>(frames * 4u);
                if ((m_wavBytes & 0xFFFFFu) < frames * 4u)
                    patchWavSizes();
            }
        }

        void patchWavSizes()
        {
            const long end = std::ftell(m_wav);
            const uint32_t riff = m_wavBytes + 36u;
            std::fseek(m_wav, 4, SEEK_SET);
            std::fwrite(&riff, 4, 1, m_wav);
            std::fseek(m_wav, 40, SEEK_SET);
            std::fwrite(&m_wavBytes, 4, 1, m_wav);
            std::fseek(m_wav, end, SEEK_SET);
            std::fflush(m_wav);
        }

        void finishRecording()
        {
            if (!m_wav)
                return;
            patchWavSizes();
            std::fclose(m_wav);
            m_wav = nullptr;
        }

        void stats(const int16_t *samples, size_t frames)
        {
            static const bool enabled = []() { const char *v = std::getenv("DOD_SPU2_STATS"); return v && v[0] == '1'; }();
            if (!enabled)
                return;
            for (size_t i = 0; i < frames * 2u; ++i)
            {
                const int32_t v = samples[i];
                m_peak = std::max<int32_t>(m_peak, v < 0 ? -v : v);
                m_sumSquares += static_cast<double>(v) * v;
            }
            m_statFrames += frames;
            if (m_statFrames >= kRate * 5u)
            {
                const double rms = std::sqrt(m_sumSquares / static_cast<double>(m_statFrames * 2u));
                size_t queued;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    queued = m_queued;
                }
                std::fprintf(stderr, "[spu2] %zu frames: peak %d rms %.0f, queued %zu ms, underruns %u, drops %u\n",
                             m_statFrames, m_peak, rms, queued * 1000u / kRate, m_underruns, m_drops);
                m_statFrames = 0u;
                m_peak = 0;
                m_sumSquares = 0.0;
            }
        }

        inline static std::atomic<DodSpu2Output *> s_active{nullptr};
        std::mutex m_mutex;
        std::vector<int16_t> m_ring = std::vector<int16_t>(kCapacity * 2u);
        size_t m_read = 0u;
        size_t m_queued = 0u;
        bool m_primed = false;
        bool m_opened = false;
        uint32_t m_fadeIn = 0u;
        double m_resamplePos = 0.0;  // position between the previous input frame and the next one
        int32_t m_prevLeft = 0;
        int32_t m_prevRight = 0;
        int32_t m_lastLeft = 0;
        int32_t m_lastRight = 0;
        AudioStream m_stream{};
        uint32_t m_underruns = 0u;
        uint32_t m_drops = 0u;
        std::FILE *m_wav = nullptr;
        bool m_wavFailed = false;
        uint32_t m_wavBytes = 0u;
        size_t m_statFrames = 0u;
        int32_t m_peak = 0;
        double m_sumSquares = 0.0;
        double m_traceLastCallback = 0.0; // audio trace, sound card thread only
        unsigned int m_traceLastFrames = 0u;
        uint32_t m_traceCallbacks = 0u;
        double m_traceMaxGap = 0.0;
        double m_traceStatusAt = 0.0;
    };

    DodSpu2Output g_dodSpu2Output;
}

bool dodAudioTraceEnabled()
{
    return g_dodAudioTrace.on();
}

void dodAudioTraceNote(const char *text)
{
    g_dodAudioTrace.event("%s", text);
}

std::string dodAudioTraceSave()
{
    return g_dodAudioTrace.save();
}

PS2IopHostAdapter::PS2IopHostAdapter(PS2Runtime &runtime)
    : m_runtime(runtime)
{
}

PS2IopHostAdapter::~PS2IopHostAdapter()
{
    g_dodSpu2Output.close(); // the IOP thread has stopped; the audio device closes after this
    std::lock_guard<std::mutex> lock(m_hostFileMutex);
    for (auto &[handle, file] : m_hostFiles)
    {
        (void)handle;
        if (file.stream)
        {
            std::fclose(file.stream);
        }
    }
    m_hostFiles.clear();
}

PS2IopHostAdapter::CallScope PS2IopHostAdapter::enterCall(R5900Context *context, uint8_t *rdram)
{
    return CallScope(*this, context, rdram);
}

bool PS2IopHostAdapter::guestRange(uint32_t address, size_t size, uint8_t *&begin) const
{
    begin = nullptr;
    // [DoD] The IOP thread never runs inside an EE call scope (m_activeRdram belongs to the EE thread).
    uint8_t *const rdram = (m_activeRdram && !ps2x::iop::onIopThread())
                               ? m_activeRdram
                               : m_runtime.memory().getRDRAM();
    if (!rdram)
    {
        return false;
    }
    if (size == 0)
    {
        begin = getMemPtr(rdram, address);
        return begin != nullptr;
    }
    if (size - 1 > std::numeric_limits<uint32_t>::max() - address)
    {
        return false;
    }
    uint8_t *const first = getMemPtr(rdram, address);
    uint8_t *const last = getMemPtr(rdram, address + static_cast<uint32_t>(size - 1));
    if (!first || !last || last < first || static_cast<size_t>(last - first) != size - 1)
    {
        return false;
    }
    begin = first;
    return true;
}

bool PS2IopHostAdapter::readGuest(uint32_t address, void *destination, size_t size) const
{
    if (!destination && size != 0)
    {
        return false;
    }
    uint8_t *source = nullptr;
    if (!guestRange(address, size, source))
    {
        return false;
    }
    if (size != 0)
    {
        std::memcpy(destination, source, size);
    }
    return true;
}

bool PS2IopHostAdapter::writeGuest(uint32_t address, const void *source, size_t size)
{
    if (!source && size != 0)
    {
        return false;
    }
    uint8_t *destination = nullptr;
    if (!guestRange(address, size, destination))
    {
        return false;
    }
    if (size != 0)
    {
        uint8_t *const rdram = m_activeRdram ? m_activeRdram : m_runtime.memory().getRDRAM();
        ps2TraceGuestRangeWrite(rdram, address, static_cast<uint32_t>(size), "IopHost::writeGuest", nullptr);
        std::memcpy(destination, source, size);
    }
    return true;
}

bool PS2IopHostAdapter::zeroGuest(uint32_t address, size_t size)
{
    uint8_t *destination = nullptr;
    if (!guestRange(address, size, destination))
    {
        return false;
    }
    if (size != 0)
    {
        uint8_t *const rdram = m_activeRdram ? m_activeRdram : m_runtime.memory().getRDRAM();
        ps2TraceGuestRangeWrite(rdram, address, static_cast<uint32_t>(size), "IopHost::zeroGuest", nullptr);
        std::memset(destination, 0, size);
    }
    return true;
}

bool PS2IopHostAdapter::normalizeGuestAddress(uint32_t address, uint32_t &normalized) const
{
    bool scratchpad = false;
    if (!ps2ResolveGuestPointer(address, normalized, scratchpad) || scratchpad)
    {
        normalized = 0;
        return false;
    }
    return true;
}

bool PS2IopHostAdapter::readIopMemory(uint32_t address, void *destination, size_t size) const
{
    return m_runtime.readIopMemory(address, destination, size);
}

bool PS2IopHostAdapter::writeIopMemory(uint32_t address, const void *source, size_t size)
{
    return m_runtime.writeIopMemory(address, source, size);
}

bool PS2IopHostAdapter::zeroIopMemory(uint32_t address, size_t size)
{
    return m_runtime.zeroIopMemory(address, size);
}

bool PS2IopHostAdapter::normalizeIopAddress(uint32_t address, uint32_t &normalized) const
{
    if (!m_runtime.isIopMemoryRange(address, 0u))
    {
        normalized = 0u;
        return false;
    }
    normalized = address & 0x1FFFFFFFu;
    return true;
}

uint32_t PS2IopHostAdapter::allocateIopHandle(ps2x::iop::IopHandleKind kind)
{
    uint8_t *const rdram = m_activeRdram
                               ? m_activeRdram
                               : m_runtime.memory().getRDRAM();
    if (!rdram)
    {
        return 0;
    }
    return kind == ps2x::iop::IopHandleKind::RpcPacket
               ? rpcAllocPacketAddr(rdram)
               : rpcAllocServerAddr(rdram);
}

uint32_t PS2IopHostAdapter::allocateGuest(uint32_t size, uint32_t alignment)
{
    return m_runtime.guestMalloc(size, alignment);
}

void PS2IopHostAdapter::freeGuest(uint32_t address)
{
    m_runtime.guestFree(address);
}

void PS2IopHostAdapter::audioOutput(const int16_t *samples, size_t frames)
{
    g_dodSpu2Output.push(samples, frames);
}

void PS2IopHostAdapter::audioCommand(uint32_t sid,
                                     uint32_t function,
                                     ps2x::iop::GuestBuffer send,
                                     ps2x::iop::GuestBuffer receive)
{
    uint8_t *sendPointer = nullptr;
    uint8_t *receivePointer = nullptr;
    if (send.address && !guestRange(send.address, send.size, sendPointer))
    {
        sendPointer = nullptr;
    }
    if (receive.address && !guestRange(receive.address, receive.size, receivePointer))
    {
        receivePointer = nullptr;
    }
    m_runtime.audioBackend().onSoundCommand(sid,
                                            function,
                                            sendPointer,
                                            send.size,
                                            receivePointer,
                                            receive.size);
}

std::string PS2IopHostAdapter::hostPath(ps2x::iop::HostPathKind kind) const
{
    const PS2Runtime::IoPaths &paths = PS2Runtime::getIoPaths();
    switch (kind)
    {
    case ps2x::iop::HostPathKind::CdRoot:
        return paths.cdRoot.string();
    case ps2x::iop::HostPathKind::CdImage:
        return paths.cdImage.string();
    case ps2x::iop::HostPathKind::HostRoot:
        return paths.hostRoot.string();
    case ps2x::iop::HostPathKind::MemoryCardRoot:
        return paths.mcRoot.string();
    case ps2x::iop::HostPathKind::ElfDirectory:
    default:
        return paths.elfDirectory.string();
    }
}

std::string PS2IopHostAdapter::translateGuestPath(std::string_view path) const
{
    const PS2Runtime::IoPaths &paths = PS2Runtime::getIoPaths();
    const PS2VfsMounts mounts{paths.hostRoot, paths.cdRoot, paths.mcRoot};
    std::filesystem::path hostPath;
    if (!m_runtime.vfs().resolveHostPath(path, mounts, hostPath))
        return {};
    return hostPath.string();
}

uint64_t PS2IopHostAdapter::openHostFile(std::string_view path)
{
    if (path.empty())
    {
        return 0u;
    }

    const std::filesystem::path hostPath{std::string(path)};
#if defined(_WIN32)
    std::FILE *stream = ::_wfopen(hostPath.c_str(), L"rb");
#else
    std::FILE *stream = std::fopen(hostPath.string().c_str(), "rb");
#endif
    if (!stream)
    {
        return 0u;
    }

    std::error_code error;
    const uint64_t size = std::filesystem::file_size(hostPath, error);
    if (error)
    {
        std::fclose(stream);
        return 0u;
    }

    std::lock_guard<std::mutex> lock(m_hostFileMutex);
    uint64_t handle = m_nextHostFileHandle++;
    if (handle == 0u)
    {
        handle = m_nextHostFileHandle++;
    }
    m_hostFiles.emplace(handle, HostFile{stream, size});
    return handle;
}

bool PS2IopHostAdapter::hostFileSize(uint64_t handle, uint64_t &size) const
{
    size = 0u;
    std::lock_guard<std::mutex> lock(m_hostFileMutex);
    const auto it = m_hostFiles.find(handle);
    if (it == m_hostFiles.end() || !it->second.stream)
    {
        return false;
    }
    size = it->second.size;
    return true;
}

bool PS2IopHostAdapter::readHostFile(uint64_t handle,
                                     uint64_t offset,
                                     void *destination,
                                     size_t size,
                                     size_t &bytesRead)
{
    bytesRead = 0u;
    if (!destination && size != 0u)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_hostFileMutex);
    const auto it = m_hostFiles.find(handle);
    if (it == m_hostFiles.end() || !it->second.stream)
    {
        return false;
    }

#if defined(_WIN32)
    if (offset > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        _fseeki64(it->second.stream, static_cast<int64_t>(offset), SEEK_SET) != 0)
#else
    if (offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) ||
        fseeko(it->second.stream, static_cast<off_t>(offset), SEEK_SET) != 0)
#endif
    {
        return false;
    }

    bytesRead = size == 0u
                    ? 0u
                    : std::fread(destination, 1u, size, it->second.stream);
    if (bytesRead < size && std::ferror(it->second.stream))
    {
        std::clearerr(it->second.stream);
        return false;
    }
    return true;
}

void PS2IopHostAdapter::closeHostFile(uint64_t handle)
{
    std::FILE *stream = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_hostFileMutex);
        const auto it = m_hostFiles.find(handle);
        if (it == m_hostFiles.end())
        {
            return;
        }
        stream = it->second.stream;
        m_hostFiles.erase(it);
    }
    if (stream)
    {
        std::fclose(stream);
    }
}

int32_t PS2IopHostAdapter::memoryCard(const ps2x::iop::MemoryCardRequest &request)
{
    using Handler = void (*)(uint8_t *, R5900Context *, PS2Runtime *);
    Handler handler = nullptr;
    switch (request.operation)
    {
    case ps2x::iop::MemoryCardOperation::Init:
        handler = ps2_stubs::sceMcInit;
        break;
    case ps2x::iop::MemoryCardOperation::GetInfo:
        handler = ps2_stubs::sceMcGetInfo;
        break;
    case ps2x::iop::MemoryCardOperation::Open:
        handler = ps2_stubs::sceMcOpen;
        break;
    case ps2x::iop::MemoryCardOperation::Close:
        handler = ps2_stubs::sceMcClose;
        break;
    case ps2x::iop::MemoryCardOperation::Seek:
        handler = ps2_stubs::sceMcSeek;
        break;
    case ps2x::iop::MemoryCardOperation::Read:
        handler = ps2_stubs::sceMcRead;
        break;
    case ps2x::iop::MemoryCardOperation::Write:
        handler = ps2_stubs::sceMcWrite;
        break;
    case ps2x::iop::MemoryCardOperation::Flush:
        handler = ps2_stubs::sceMcFlush;
        break;
    case ps2x::iop::MemoryCardOperation::Chdir:
        handler = ps2_stubs::sceMcChdir;
        break;
    case ps2x::iop::MemoryCardOperation::GetDir:
        handler = ps2_stubs::sceMcGetDir;
        break;
    case ps2x::iop::MemoryCardOperation::SetFileInfo:
        handler = ps2_stubs::sceMcSetFileInfo;
        break;
    case ps2x::iop::MemoryCardOperation::Delete:
        handler = ps2_stubs::sceMcDelete;
        break;
    case ps2x::iop::MemoryCardOperation::Format:
        handler = ps2_stubs::sceMcFormat;
        break;
    case ps2x::iop::MemoryCardOperation::Unformat:
        handler = ps2_stubs::sceMcUnformat;
        break;
    case ps2x::iop::MemoryCardOperation::Mkdir:
        handler = ps2_stubs::sceMcMkdir;
        break;
    }
    if (!handler)
    {
        return -1;
    }

    R5900Context context{};
    for (size_t i = 0; i < 4u; ++i)
    {
        setRegU32(&context, static_cast<int>(4 + i), request.arguments[i]);
    }

    // EE n32 ABI: the fifth argument travels in $t0, matching the sceMc* stubs.
    setRegU32(&context, 8, request.arguments[4]);

    handler(m_activeRdram ? m_activeRdram : m_runtime.memory().getRDRAM(),
            &context,
            &m_runtime);
    return ps2_stubs::getMemoryCardDebugSnapshot().lastResult;
}

bool PS2IopHostAdapter::hasGuestFunction(uint32_t address) const
{
    return m_runtime.hasFunction(address);
}

bool PS2IopHostAdapter::invokeGuestFunction(uint64_t callToken,
                                            uint32_t address,
                                            uint32_t a0,
                                            uint32_t a1,
                                            uint32_t a2,
                                            uint32_t a3,
                                            uint32_t *resultAddress)
{
    (void)callToken;
    (void)address;
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    if (resultAddress)
    {
        *resultAddress = 0u;
    }
    return false;
}

namespace ps2_syscalls
{
    bool findEeRpcServer(uint8_t *rdram, uint32_t sid, uint32_t &server, uint32_t &buff, uint32_t &cbuff);
}

bool PS2IopHostAdapter::lookupEeRpcServer(uint32_t sid, uint32_t &server, uint32_t &buff, uint32_t &cbuff) const
{
    uint8_t *const rdram = m_activeRdram ? m_activeRdram : m_runtime.memory().getRDRAM();
    return ps2_syscalls::findEeRpcServer(rdram, sid, server, buff, cbuff);
}

namespace
{
    // [DoD] The adapter whose posted-work queue EeScheduler drains (ps2_iop_post.h). One runtime per process.
    std::atomic<PS2IopHostAdapter *> g_postAdapter{nullptr};
}

void PS2IopHostAdapter::setPostSifCommands(bool enable)
{
    m_postSifCommands.store(enable, std::memory_order_release);
    if (enable)
        g_postAdapter.store(this, std::memory_order_release);
}

void PS2IopHostAdapter::drainPosted()
{
    // EE thread. Commands posted while this runs get their own wake-up event.
    std::vector<PostedSifCommand> batch;
    {
        std::lock_guard<std::mutex> lock(m_postMutex);
        batch.swap(m_posted);
        m_postWakeQueued = false;
    }
    uint8_t *const rdram = m_runtime.memory().getRDRAM();
    for (const PostedSifCommand &command : batch)
        (void)ps2_stubs::dispatchSifCommand(rdram, &m_runtime, command.commandId, command.packet.data(), command.packet.size());
}

void ps2IopDrainPosted(PS2Runtime &runtime)
{
    (void)runtime;
    if (PS2IopHostAdapter *adapter = g_postAdapter.load(std::memory_order_acquire))
        adapter->drainPosted();
}

bool PS2IopHostAdapter::sendSifCommand(uint32_t commandId,
                                       const void *packet,
                                       size_t packetSize)
{
    if (m_postSifCommands.load(std::memory_order_acquire))
    {
        // [DoD] Threaded IOP: queue the packet (in order) for the EE thread. dispatchSifCommand queues an EE
        // invocation, which only the EE thread may do. Returns true like a delivered command.
        if (!packet || packetSize < 16u || packetSize > 112u)
            return false;
        bool wake = false;
        {
            std::lock_guard<std::mutex> lock(m_postMutex);
            PostedSifCommand posted;
            posted.commandId = commandId;
            posted.packet.assign(static_cast<const uint8_t *>(packet), static_cast<const uint8_t *>(packet) + packetSize);
            m_posted.push_back(std::move(posted));
            if (!m_postWakeQueued)
            {
                m_postWakeQueued = true;
                wake = true;
            }
        }
        if (wake)
            m_runtime.postEeEvent(EeEvent{EeEventType::Dmac, kIopPostedWorkEventId, 0u});
        return true;
    }
    uint8_t *const rdram = m_activeRdram
                               ? m_activeRdram
                               : m_runtime.memory().getRDRAM();
    return ps2_stubs::dispatchSifCommand(rdram,
                                         &m_runtime,
                                         commandId,
                                         packet,
                                         packetSize);
}

void PS2IopHostAdapter::log(ps2x::iop::LogLevel level, std::string_view message)
{
    const char *prefix = "[ps2xIOP]";
    if (level == ps2x::iop::LogLevel::Warning)
    {
        prefix = "[ps2xIOP:warning]";
    }
    else if (level == ps2x::iop::LogLevel::Error)
    {
        prefix = "[ps2xIOP:error]";
    }
    std::cerr << prefix << ' ' << message << std::endl;
}
