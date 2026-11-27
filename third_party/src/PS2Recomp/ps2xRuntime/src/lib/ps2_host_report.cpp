// [Fixer] Performance overlay and diagnostics package (ps2_host_report.h). No raylib here (windows.h is included).
#include "ps2_host_report.h"

#include "gs/dod_pcsx2_gs.h"
#include "ps2_host_cpu.h"
#include "ps2_host_os.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace ps2_host_report
{
    namespace
    {
        namespace fs = std::filesystem;
        using Clock = std::chrono::steady_clock;

        std::atomic<bool> g_overlay{false};
        bool g_overlayShown = false; // window thread
        bool g_envChecked = false;   // window thread
        std::mutex g_lock;
        std::string g_last;
        std::deque<std::string> g_history; // the last minute of samples, for the package
        std::string g_status;
        std::atomic<bool> g_busy{false};
        std::string g_summary; // the menu's status lines, refreshed with each sample (the menu draws every frame)
        std::string g_gpu;     // window thread: the GPU line once logs\gpu.txt exists

        struct Sampler
        {
            bool started = false;
            Clock::time_point at;
            uint64_t presents = 0, flips = 0;
            double ee = -1.0, gs = -1.0;
        } g_sampler;

        fs::path exeDir()
        {
#if defined(_WIN32)
            wchar_t path[MAX_PATH] = {};
            const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
            if (n > 0 && n < MAX_PATH)
                return fs::path(path).parent_path();
#endif
            std::error_code ec;
            return fs::current_path(ec);
        }

        std::string readFile(const fs::path &path, size_t limit = static_cast<size_t>(-1));
        std::string cpuLine();

        std::string percent(double cpuNow, double cpuThen, double seconds)
        {
            if (cpuNow < 0.0 || cpuThen < 0.0 || seconds <= 0.0)
                return "?";
            char text[16];
            std::snprintf(text, sizeof(text), "%.0f%%", std::max(0.0, (cpuNow - cpuThen) / seconds * 100.0));
            return text;
        }

        void sample()
        {
            const Clock::time_point now = Clock::now();
            const uint64_t presents = dodpcsx2::presents();
            const uint64_t flips = dodpcsx2::flips();
            const double ee = ps2host::threadCpuSeconds(ps2host::ThreadRole::Game);
            const double gs = ps2host::threadCpuSeconds(ps2host::ThreadRole::Gs);
            if (g_sampler.started)
            {
                const double seconds = std::chrono::duration<double>(now - g_sampler.at).count();
                char line[160];
                std::snprintf(line, sizeof(line), "%.0f fps (%.0f fields/s) | EE %s | GS %s | %s %dx",
                              static_cast<double>(flips - g_sampler.flips) / seconds,
                              static_cast<double>(presents - g_sampler.presents) / seconds,
                              percent(ee, g_sampler.ee, seconds).c_str(), percent(gs, g_sampler.gs, seconds).c_str(),
                              dodpcsx2::active() ? "D3D11" : "CPU raster", dodpcsx2::scale());
                std::lock_guard<std::mutex> lock(g_lock);
                g_last = line;
                g_history.push_back(line);
                while (g_history.size() > 60u)
                    g_history.pop_front();
            }
            g_sampler = Sampler{true, now, presents, flips, ee, gs};
            static const std::string cpu = cpuLine();
            if (g_gpu.empty())
            {
                std::string text = readFile(exeDir() / "logs" / "gpu.txt", 2048);
                if (const size_t cut = text.find_first_of("\r\n"); cut != std::string::npos)
                    text.resize(cut);
                g_gpu = text;
            }
            std::string summary = "CPU: " + cpu + "\nGPU: " + (g_gpu.empty() ? std::string("?") : g_gpu) + "\n" +
                                  (dodpcsx2::active() ? "D3D11" : "CPU raster") + " " +
                                  std::to_string(dodpcsx2::scale()) + "x";
            std::lock_guard<std::mutex> lock(g_lock);
            g_summary = std::move(summary);
        }

        std::string readFile(const fs::path &path, size_t limit)
        {
            std::ifstream in(path, std::ios::binary); // MSVC opens shared (the launcher keeps last-run.log open)
            if (!in)
                return {};
            std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (data.size() > limit)
                data.erase(0, data.size() - limit); // keep the end of a long log
            return data;
        }

        std::string osLine()
        {
#if defined(_WIN32)
            struct OsVersion
            {
                ULONG size, major, minor, build, platform;
                WCHAR csd[128];
            } v{};
            v.size = sizeof(v);
            using RtlGetVersionFn = LONG(WINAPI *)(OsVersion *);
            if (const auto get = reinterpret_cast<RtlGetVersionFn>(
                    GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))
            {
                if (get(&v) == 0)
                {
                    char text[64];
                    std::snprintf(text, sizeof(text), "Windows %lu.%lu build %lu", v.major, v.minor, v.build);
                    return text;
                }
            }
            return "Windows (version unknown)";
#else
            return "non-Windows host";
#endif
        }

        std::string memoryLine()
        {
#if defined(_WIN32)
            MEMORYSTATUSEX m{};
            m.dwLength = sizeof(m);
            if (GlobalMemoryStatusEx(&m))
            {
                char text[80];
                std::snprintf(text, sizeof(text), "%.1f GB total, %.1f GB available",
                              m.ullTotalPhys / 1073741824.0, m.ullAvailPhys / 1073741824.0);
                return text;
            }
#endif
            return "unknown";
        }

        std::string cpuLine()
        {
            const ps2host::CpuReport r = ps2host::cpuReport();
            char text[256];
            std::snprintf(text, sizeof(text), "%s | %u logical | SSE4.1 %s AVX %s AVX2 %s",
                          r.brand.empty() ? r.vendor.c_str() : r.brand.c_str(), r.logicalCores,
                          r.sse41 ? "yes" : "NO", r.osAvx ? "yes" : "no", r.avx2 ? "yes" : "no");
            return text;
        }

        std::string gpuLines(const fs::path &dir)
        {
            std::string text = readFile(dir / "logs" / "gpu.txt", 2048);
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
                text.pop_back();
            return text.empty() ? "unknown (logs\\gpu.txt not written yet)" : text;
        }

        std::string timestamp(bool forFileName)
        {
            const std::time_t t = std::time(nullptr);
            std::tm local{};
#if defined(_WIN32)
            localtime_s(&local, &t);
#else
            localtime_r(&t, &local);
#endif
            char text[32];
            std::strftime(text, sizeof(text), forFileName ? "%Y%m%d-%H%M%S" : "%Y-%m-%d %H:%M:%S", &local);
            return text;
        }

        // ---- ZIP (stored entries, UTF-8 names), enough for a support package.
        uint32_t crc32(const std::string &data)
        {
            static uint32_t table[256];
            static bool ready = false;
            if (!ready)
            {
                for (uint32_t i = 0; i < 256u; ++i)
                {
                    uint32_t c = i;
                    for (int k = 0; k < 8; ++k)
                        c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    table[i] = c;
                }
                ready = true;
            }
            uint32_t crc = 0xFFFFFFFFu;
            for (const unsigned char byte : data)
                crc = table[(crc ^ byte) & 0xFFu] ^ (crc >> 8);
            return crc ^ 0xFFFFFFFFu;
        }

        void put16(std::string &out, uint32_t v)
        {
            out.push_back(static_cast<char>(v & 0xFFu));
            out.push_back(static_cast<char>((v >> 8) & 0xFFu));
        }

        void put32(std::string &out, uint32_t v)
        {
            put16(out, v & 0xFFFFu);
            put16(out, v >> 16);
        }

        bool writeZip(const fs::path &path, const std::vector<std::pair<std::string, std::string>> &files)
        {
            const std::time_t t = std::time(nullptr);
            std::tm local{};
#if defined(_WIN32)
            localtime_s(&local, &t);
#else
            localtime_r(&t, &local);
#endif
            const uint32_t dosTime = (local.tm_hour << 11) | (local.tm_min << 5) | (local.tm_sec / 2);
            const uint32_t dosDate = ((local.tm_year + 1900 - 1980) << 9) | ((local.tm_mon + 1) << 5) | local.tm_mday;
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out)
                return false;
            std::string central;
            uint32_t offset = 0;
            for (const auto &[name, data] : files)
            {
                const uint32_t crc = crc32(data);
                const uint32_t size = static_cast<uint32_t>(data.size());
                std::string header;
                put32(header, 0x04034B50u);
                put16(header, 20u);
                put16(header, 0x0800u); // UTF-8 names
                put16(header, 0u);      // stored
                put16(header, dosTime);
                put16(header, dosDate);
                put32(header, crc);
                put32(header, size);
                put32(header, size);
                put16(header, static_cast<uint32_t>(name.size()));
                put16(header, 0u);
                header += name;
                out.write(header.data(), static_cast<std::streamsize>(header.size()));
                out.write(data.data(), static_cast<std::streamsize>(data.size()));
                put32(central, 0x02014B50u);
                put16(central, 20u);
                put16(central, 20u);
                put16(central, 0x0800u);
                put16(central, 0u);
                put16(central, dosTime);
                put16(central, dosDate);
                put32(central, crc);
                put32(central, size);
                put32(central, size);
                put16(central, static_cast<uint32_t>(name.size()));
                put16(central, 0u);
                put16(central, 0u);
                put16(central, 0u);
                put16(central, 0u);
                put32(central, 0u);
                put32(central, offset);
                central += name;
                offset += static_cast<uint32_t>(header.size()) + size;
            }
            std::string end;
            put32(end, 0x06054B50u);
            put16(end, 0u);
            put16(end, 0u);
            put16(end, static_cast<uint32_t>(files.size()));
            put16(end, static_cast<uint32_t>(files.size()));
            put32(end, static_cast<uint32_t>(central.size()));
            put32(end, offset);
            put16(end, 0u);
            out.write(central.data(), static_cast<std::streamsize>(central.size()));
            out.write(end.data(), static_cast<std::streamsize>(end.size()));
            return static_cast<bool>(out);
        }

        void setStatus(const std::string &text)
        {
            std::lock_guard<std::mutex> lock(g_lock);
            g_status = text;
        }

        void writePackage(const std::string &settingsFile)
        {
            const fs::path dir = exeDir();
            const fs::path logs = dir / "logs";
            std::error_code ec;
            fs::create_directories(logs, ec);
            std::vector<std::pair<std::string, std::string>> files;

            std::string summary = "PS2Recomp-Fixer diagnostics package\n";
            summary += "written: " + timestamp(false) + " (local time)\n";
            {
                std::error_code e2;
                const fs::path exe = [] {
#if defined(_WIN32)
                    wchar_t path[MAX_PATH] = {};
                    GetModuleFileNameW(nullptr, path, MAX_PATH);
                    return fs::path(path);
#else
                    return fs::path();
#endif
                }();
                const auto bytes = fs::file_size(exe, e2);
                summary += "exe: " + exe.filename().string() + " (" + std::to_string(e2 ? 0 : bytes) + " bytes)\n";
            }
            summary += "cpu: " + cpuLine() + "\n";
            summary += "os: " + osLine() + "\n";
            summary += "memory: " + memoryLine() + "\n";
            summary += std::string("renderer: ") + (dodpcsx2::active() ? "D3D11 (pcsx2gs.dll)" : "CPU raster") +
                       ", internal scale " + std::to_string(dodpcsx2::scale()) + "x\n";
            summary += std::string("perf overlay: ") + (g_overlay.load() ? "on" : "off") + "\n";
            summary += "crash dumps written by this run: " + std::to_string(ps2host::crashDumpsWritten()) + "\n";
            {
                std::lock_guard<std::mutex> lock(g_lock);
                summary += "last samples (one per second, newest last):\n";
                for (const std::string &line : g_history)
                    summary += "  " + line + "\n";
            }
            summary += "gpu.txt:\n  " + gpuLines(dir) + "\n";
            files.emplace_back("summary.txt", summary);

            const auto add = [&](const fs::path &path, const std::string &name, size_t limit) {
                std::error_code e3;
                if (!fs::is_regular_file(path, e3))
                    return;
                std::string data = readFile(path, limit);
                if (!data.empty())
                    files.emplace_back(name, std::move(data));
            };
            add(logs / "last-run.log", "logs/last-run.log", 16u << 20);
            add(logs / "gpu.txt", "logs/gpu.txt", 1u << 20);
            if (!settingsFile.empty())
                add(dir / settingsFile, settingsFile, 1u << 20);
            std::vector<fs::path> dumps;
            for (const auto &entry : fs::directory_iterator(logs, ec))
            {
                const std::string name = entry.path().filename().string();
                if (name.rfind("crash-", 0) == 0 && entry.path().extension() == ".dmp")
                    dumps.push_back(entry.path());
            }
            std::sort(dumps.begin(), dumps.end());
            for (size_t i = dumps.size() > 2u ? dumps.size() - 2u : 0u; i < dumps.size(); ++i)
                add(dumps[i], "logs/" + dumps[i].filename().string(), 256u << 20);

            const std::string name = "diagnostics-" + timestamp(true) + ".zip";
            if (writeZip(logs / name, files))
            {
                setStatus("Written to logs\\" + name);
                std::fprintf(stderr, "[report] diagnostics package logs\\%s (%zu files)\n", name.c_str(), files.size());
            }
            else
            {
                setStatus("Export failed: cannot write the logs folder");
                std::fprintf(stderr, "[report] diagnostics package failed (logs folder not writable)\n");
            }
        }
    }

    void setOverlay(bool on)
    {
        g_overlay.store(on);
        g_envChecked = true; // an explicit choice wins over PS2_PERF_OVERLAY
    }

    bool overlay()
    {
        return g_overlay.load();
    }

    void hostTick()
    {
        if (!g_envChecked)
        {
            g_envChecked = true;
            const char *v = std::getenv("PS2_PERF_OVERLAY");
            if (v && v[0] == '1')
                g_overlay.store(true);
        }
        const Clock::time_point now = Clock::now();
        if (!g_sampler.started || now - g_sampler.at >= std::chrono::seconds(1))
        {
            sample();
            if (g_overlay.load())
            {
                const std::string line = lastSample();
                if (!line.empty())
                {
                    dodpcsx2::setOsdLine(line.c_str());
                    g_overlayShown = true;
                }
            }
        }
        if (!g_overlay.load() && g_overlayShown)
        {
            dodpcsx2::setOsdLine("");
            g_overlayShown = false;
        }
    }

    std::string lastSample()
    {
        std::lock_guard<std::mutex> lock(g_lock);
        return g_last;
    }

    std::string hostSummary()
    {
        std::lock_guard<std::mutex> lock(g_lock);
        return g_summary;
    }

    void exportDiagnostics(const std::string &settingsFile)
    {
        if (g_busy.exchange(true))
            return;
        setStatus("Writing the diagnostics package...");
        std::thread([settingsFile] {
            writePackage(settingsFile);
            g_busy.store(false);
        }).detach();
    }

    std::string exportStatus()
    {
        std::lock_guard<std::mutex> lock(g_lock);
        return g_status;
    }
}
