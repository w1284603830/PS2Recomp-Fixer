// [Fixer] Host OS scheduling and crash reporting (ps2_host_os.h). Windows only; other hosts get no-ops.
#include "ps2_host_os.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <exception>
#include <string>
#include <vector>
#include <algorithm>
#endif

#if defined(_WIN32)
// [Fixer] Dual-GPU laptops: ask the NVIDIA Optimus / AMD PowerXpress drivers for the discrete GPU. The PCSX2 GS already
// picks the high-performance adapter through DXGI on Windows 10 1803+; these exports cover older systems and the
// OpenGL host presentation used by the CPU-raster fallback. Exported from the EXE (this object is always linked).
extern "C"
{
    __declspec(dllexport) unsigned long NvOptimusEnablement = 1;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

namespace
{
    std::atomic<uint32_t> g_dumpsWritten{0};
#if defined(_WIN32)
    std::atomic<HANDLE> g_roleThreads[3] = {}; // ThreadRole -> a handle of the thread that took it (perf overlay)
#endif

    bool priorityRaiseAllowed()
    {
        const char *v = std::getenv("PS2_THREAD_PRIORITY");
        if (v && v[0] == '0')
            return false;
        return std::thread::hardware_concurrency() >= 4u;
    }

#if defined(_WIN32)
    // Win32 output without the CRT stream locks (a crash may happen while one is held).
    void writeStderr(const char *text)
    {
        const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
        if (err && err != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            WriteFile(err, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
        }
    }

    using MiniDumpWriteDumpFn = BOOL(WINAPI *)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                               PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
                                               PMINIDUMP_CALLBACK_INFORMATION);
    MiniDumpWriteDumpFn g_writeDump = nullptr;
    wchar_t g_dumpDir[MAX_PATH] = {};
    std::atomic<bool> g_inCrash{false};
    std::atomic<bool> g_crashInstalled{false};
    LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;
    constexpr size_t kKeepDumps = 4u; // a crash loop must not fill the disk

    void pruneOldDumps()
    {
        std::wstring pattern = std::wstring(g_dumpDir) + L"\\crash-*.dmp";
        WIN32_FIND_DATAW data;
        const HANDLE find = FindFirstFileW(pattern.c_str(), &data);
        if (find == INVALID_HANDLE_VALUE)
            return;
        std::vector<std::wstring> names;
        do
            names.emplace_back(data.cFileName);
        while (FindNextFileW(find, &data));
        FindClose(find);
        std::sort(names.begin(), names.end()); // crash-YYYYMMDD-HHMMSS: name order is time order
        while (names.size() >= kKeepDumps)
        {
            DeleteFileW((std::wstring(g_dumpDir) + L"\\" + names.front()).c_str());
            names.erase(names.begin());
        }
    }

    void writeDump(EXCEPTION_POINTERS *exception, const char *reason)
    {
        char line[512];
        if (exception && exception->ExceptionRecord)
        {
            const EXCEPTION_RECORD &record = *exception->ExceptionRecord;
            const void *address = record.ExceptionAddress;
            HMODULE module = nullptr;
            wchar_t modulePath[MAX_PATH] = L"?";
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   static_cast<LPCWSTR>(address), &module) && module)
                GetModuleFileNameW(module, modulePath, MAX_PATH);
            const wchar_t *base = modulePath;
            for (const wchar_t *p = modulePath; *p; ++p)
                if (*p == L'\\' || *p == L'/')
                    base = p + 1;
            std::snprintf(line, sizeof(line), "[crash] %s: exception 0x%08lX at %ls+0x%llX (thread %lu)\n", reason,
                          static_cast<unsigned long>(record.ExceptionCode), base,
                          static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) -
                                                          reinterpret_cast<uintptr_t>(module)),
                          static_cast<unsigned long>(GetCurrentThreadId()));
        }
        else
        {
            std::snprintf(line, sizeof(line), "[crash] %s (thread %lu)\n", reason,
                          static_cast<unsigned long>(GetCurrentThreadId()));
        }
        writeStderr(line);
        if (!g_writeDump || !g_dumpDir[0])
            return;
        CreateDirectoryW(g_dumpDir, nullptr);
        pruneOldDumps();
        SYSTEMTIME now;
        GetLocalTime(&now);
        wchar_t path[MAX_PATH + 64];
        swprintf(path, sizeof(path) / sizeof(path[0]), L"%ls\\crash-%04u%02u%02u-%02u%02u%02u.dmp", g_dumpDir,
                 now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
        const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return;
        MINIDUMP_EXCEPTION_INFORMATION info{};
        info.ThreadId = GetCurrentThreadId();
        info.ExceptionPointers = exception;
        info.ClientPointers = FALSE;
        const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                                                              MiniDumpWithIndirectlyReferencedMemory);
        const BOOL ok = g_writeDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                                    exception ? &info : nullptr, nullptr, nullptr);
        CloseHandle(file);
        if (ok)
        {
            g_dumpsWritten.fetch_add(1u, std::memory_order_relaxed);
            std::snprintf(line, sizeof(line), "[crash] minidump: %ls\n", path);
        }
        else
        {
            DeleteFileW(path);
            std::snprintf(line, sizeof(line), "[crash] minidump failed (error %lu)\n",
                          static_cast<unsigned long>(GetLastError()));
        }
        writeStderr(line);
    }

    LONG WINAPI crashFilter(EXCEPTION_POINTERS *exception)
    {
        if (!g_inCrash.exchange(true))
            writeDump(exception, "unhandled exception");
        return g_previousFilter ? g_previousFilter(exception) : EXCEPTION_CONTINUE_SEARCH;
    }

    [[noreturn]] void onTerminate()
    {
        if (!g_inCrash.exchange(true))
            writeDump(nullptr, "std::terminate (uncaught C++ exception)");
        std::abort();
    }
#endif
}

namespace ps2host
{
    void configureHostProcess()
    {
#if defined(_WIN32)
        static std::atomic<bool> done{false};
        if (done.exchange(true))
            return;
        // PROCESS_POWER_THROTTLING_STATE; constants spelled out for older SDKs.
        struct PowerThrottlingState
        {
            ULONG Version, ControlMask, StateMask;
        };
        constexpr int kProcessPowerThrottling = 4;
        constexpr ULONG kExecutionSpeed = 0x1u, kIgnoreTimerResolution = 0x4u;
        using SetProcessInformationFn = BOOL(WINAPI *)(HANDLE, int, LPVOID, DWORD);
        const auto setInfo = reinterpret_cast<SetProcessInformationFn>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation"));
        const char *result = "unavailable (Windows 7/8)";
        if (setInfo)
        {
            PowerThrottlingState state{1u, kExecutionSpeed | kIgnoreTimerResolution, 0u};
            if (setInfo(GetCurrentProcess(), kProcessPowerThrottling, &state, sizeof(state)))
                result = "off (no EcoQoS, timer resolution kept when hidden)";
            else
            {
                state.ControlMask = kExecutionSpeed; // Windows 10 before 21H2 rejects the timer flag
                result = setInfo(GetCurrentProcess(), kProcessPowerThrottling, &state, sizeof(state))
                             ? "off (no EcoQoS)"
                             : "unchanged (SetProcessInformation failed)";
            }
        }
        std::fprintf(stderr, "[host] power throttling %s; EE/GS threads %s priority\n", result,
                     priorityRaiseAllowed() ? "above-normal" : "normal");
#endif
    }

    void setCurrentThreadRole(ThreadRole role)
    {
#if defined(_WIN32)
        if ((role == ThreadRole::Game || role == ThreadRole::Gs) && priorityRaiseAllowed())
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        HANDLE self = nullptr;
        if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self,
                            THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0))
        {
            if (const HANDLE previous = g_roleThreads[static_cast<int>(role)].exchange(self))
                CloseHandle(previous);
        }
        if (role == ThreadRole::Gs) // named for per-thread reports (the EE and IOP threads name themselves)
        {
            using SetThreadDescriptionFn = HRESULT(WINAPI *)(HANDLE, PCWSTR);
            if (const auto describe = reinterpret_cast<SetThreadDescriptionFn>(
                    GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")))
                describe(GetCurrentThread(), L"dod GS");
        }
#else
        (void)role;
#endif
    }

    void installCrashHandler()
    {
#if defined(_WIN32)
        if (g_crashInstalled.exchange(true))
            return;
        // Loaded now: LoadLibrary inside a crashing process can deadlock on the loader lock.
        if (const HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll"))
            g_writeDump = reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
        // <exe folder>\logs, where the launcher keeps last-run.log (the working directory may differ).
        const DWORD n = GetModuleFileNameW(nullptr, g_dumpDir, MAX_PATH - 8);
        wchar_t *slash = nullptr;
        for (wchar_t *p = g_dumpDir; n > 0 && n < MAX_PATH - 8 && *p; ++p)
            if (*p == L'\\' || *p == L'/')
                slash = p;
        if (slash)
            wcscpy_s(slash, MAX_PATH - static_cast<size_t>(slash - g_dumpDir), L"\\logs");
        else
            g_dumpDir[0] = 0;
        g_previousFilter = SetUnhandledExceptionFilter(crashFilter);
        std::set_terminate(onTerminate);
        // PS2_TEST_CRASH=<seconds> (tests only): an access violation on a worker thread after that many seconds.
        if (const char *v = std::getenv("PS2_TEST_CRASH"); v && std::atoi(v) > 0)
        {
            const int seconds = std::atoi(v);
            std::thread([seconds] {
                std::this_thread::sleep_for(std::chrono::seconds(seconds));
                volatile int *const nowhere = nullptr;
                *nowhere = 1;
            }).detach();
        }
#endif
    }

    uint32_t crashDumpsWritten()
    {
        return g_dumpsWritten.load(std::memory_order_relaxed);
    }

    GpuReport gpuReport(const std::string &adapterName)
    {
        GpuReport r;
#if defined(_WIN32)
        using CreateFactoryFn = HRESULT(WINAPI *)(REFIID, void **);
        const HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
        const auto create = dxgi ? reinterpret_cast<CreateFactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
        IDXGIFactory1 *factory = nullptr;
        if (!create || FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) || !factory)
            return r;
        const auto utf8 = [](const wchar_t *text) {
            char out[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, text, -1, out, sizeof(out) - 1, nullptr, nullptr);
            std::string s(out);
            while (!s.empty() && s.back() == ' ')
                s.pop_back();
            while (!s.empty() && s.front() == ' ')
                s.erase(s.begin());
            return s;
        };
        IDXGIAdapter1 *chosen = nullptr;
        const auto take = [&](IDXGIAdapter1 *adapter) {
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(adapter->GetDesc1(&desc)))
                return false;
            adapter->AddRef();
            chosen = adapter;
            r.found = true;
            r.name = utf8(desc.Description);
            r.vendorId = desc.VendorId;
            r.dedicatedBytes = desc.DedicatedVideoMemory;
            r.software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            return true;
        };
        std::string wanted = adapterName;
        if (wanted == "auto")
            wanted.clear();
        for (UINT i = 0; !wanted.empty() && !r.found; ++i)
        {
            IDXGIAdapter1 *adapter = nullptr;
            if (FAILED(factory->EnumAdapters1(i, &adapter)))
                break;
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc)) && utf8(desc.Description) == wanted)
                take(adapter);
            adapter->Release();
        }
        IDXGIFactory6 *factory6 = nullptr;
        if (!r.found && SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory6), reinterpret_cast<void **>(&factory6))))
        {
            for (UINT i = 0; !r.found; ++i)
            {
                IDXGIAdapter1 *adapter = nullptr;
                if (FAILED(factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                                __uuidof(IDXGIAdapter1), reinterpret_cast<void **>(&adapter))))
                    break;
                DXGI_ADAPTER_DESC1 desc{};
                if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
                    take(adapter);
                adapter->Release();
            }
            factory6->Release();
        }
        if (!r.found)
        {
            IDXGIAdapter1 *adapter = nullptr;
            if (SUCCEEDED(factory->EnumAdapters1(0, &adapter)))
            {
                take(adapter);
                adapter->Release();
            }
        }
        // Integrated GPUs share system memory (UMA). DXGI has no such flag and an APU may report a 2 GB BIOS carve-out as
        // "dedicated", so ask D3D12 when it is available (Windows 10); the 1 GiB rule is the fallback.
        if (chosen && !r.software)
        {
            using CreateDeviceFn = HRESULT(WINAPI *)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
            const HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
            const auto createDevice =
                d3d12 ? reinterpret_cast<CreateDeviceFn>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
            ID3D12Device *device = nullptr;
            if (createDevice && SUCCEEDED(createDevice(chosen, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                                                       reinterpret_cast<void **>(&device))) && device)
            {
                D3D12_FEATURE_DATA_ARCHITECTURE arch{};
                if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &arch, sizeof(arch))))
                {
                    r.umaKnown = true;
                    r.uma = arch.UMA != FALSE;
                }
                device->Release();
            }
        }
        if (chosen)
            chosen->Release();
        factory->Release();
#else
        (void)adapterName;
#endif
        return r;
    }

    int autoScaleCap(const GpuReport &gpu, const char **tier)
    {
        const char *kind = !gpu.found ? "discrete" // unknown: keep the display-based choice up to 4x
                           : gpu.software ? "software"
                           : gpu.umaKnown ? (gpu.uma ? "integrated" : "discrete")
                           : gpu.dedicatedBytes < (1ull << 30) ? "integrated"
                                                                : "discrete";
        if (const char *v = std::getenv("PS2_GPU_TIER"); v && *v)
            kind = std::strcmp(v, "integrated") == 0 ? "integrated" : std::strcmp(v, "software") == 0 ? "software"
                                                                                                       : "discrete";
        if (tier)
            *tier = kind;
        return std::strcmp(kind, "software") == 0 ? 1 : std::strcmp(kind, "integrated") == 0 ? 2 : 4;
    }

    double threadCpuSeconds(ThreadRole role)
    {
#if defined(_WIN32)
        const HANDLE thread = g_roleThreads[static_cast<int>(role)].load();
        FILETIME created, exited, kernel, user;
        if (!thread || !GetThreadTimes(thread, &created, &exited, &kernel, &user))
            return -1.0;
        const auto seconds = [](const FILETIME &t) {
            return static_cast<double>((static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime) * 1e-7;
        };
        return seconds(kernel) + seconds(user);
#else
        (void)role;
        return -1.0;
#endif
    }
}
