#ifndef PS2_HOST_OS_H
#define PS2_HOST_OS_H

// [Fixer] Host OS scheduling and crash reporting, game-neutral.
//
// Power throttling: Windows 10/11 may run a process under EcoQoS (lower clocks, efficiency cores on hybrid CPUs)
// when its window is not in the foreground, on battery or in power-saver plans, and Windows 11 may also ignore its
// timer-resolution request while the window is hidden. The emulated EE has a real-time budget per field, so the port
// opts out of both (SetProcessInformation(ProcessPowerThrottling), as games and media players do).
// Thread roles: the EE (game) and GS threads run one step above normal priority on hosts with at least 4 logical
// CPUs, so background work does not preempt them mid-field; on smaller hosts they stay normal so the window, audio and
// IOP threads are never starved. PS2_THREAD_PRIORITY=0 keeps every thread normal.
// Crash reports: an unhandled exception writes logs\crash-<time>.dmp (a minidump: threads, stacks, modules) and one
// "[crash]" line with the exception code and module+offset, so "it closes on my PC" reports carry evidence.

#include <cstdint>
#include <string>

namespace ps2host
{
    enum class ThreadRole
    {
        Game, // the EE thread (generated code, scheduler)
        Gs,   // the GS worker (pcsx2gs.dll)
        Iop,  // the threaded IOP
    };

    // Once per process; logs one "[host] ..." line. Safe to call more than once.
    void configureHostProcess();
    // Applies the role's priority to the calling thread (see above).
    void setCurrentThreadRole(ThreadRole role);
    // Installs the unhandled-exception minidump writer (Windows). Safe to call more than once.
    void installCrashHandler();
    // Number of crash dumps written by this process (0 normally); for diagnostics exports.
    uint32_t crashDumpsWritten();
    // CPU time (user + kernel, seconds) of the last thread that took this role; negative when none did (perf overlay).
    double threadCpuSeconds(ThreadRole role);

    // [Fixer] The GPU the PCSX2 GS renders with: the adapter named in the GS options (adapter=..., the launcher's GPU
    // choice) when present, else the first hardware adapter by Windows' high-performance preference (the GS's own
    // automatic choice, D3D::GetAdapterByName), else DXGI adapter 0 (Windows before 10 1803).
    struct GpuReport
    {
        bool found = false;
        std::string name;
        uint32_t vendorId = 0;
        uint64_t dedicatedBytes = 0; // DXGI DedicatedVideoMemory: integrated GPUs report 128-512 MB
        bool software = false;       // Microsoft Basic Render Driver (no GPU driver)
        bool umaKnown = false;       // D3D12 answered the architecture query
        bool uma = false;            // unified memory: an integrated GPU
    };
    GpuReport gpuReport(const std::string &adapterName = std::string());
    // Ceiling for gs_scale = auto by GPU class: 4 on a discrete GPU (5x and more at 4K costs a lot for little), 2 on an
    // integrated GPU (D3D12 reports unified memory; without D3D12: under 1 GiB dedicated memory), 1 on the software
    // renderer. tier receives "discrete" / "integrated" / "software". PS2_GPU_TIER=integrated|discrete|software
    // replaces the detected class (tests).
    int autoScaleCap(const GpuReport &gpu, const char **tier = nullptr);
}

#endif
