// Onimusha: Dawn of Dreams host entry point (replaces ps2xRuntime/src/main.cpp for this game).
//
// Settings come from dod.ini next to the executable (flat "key = value" lines, written by the launcher);
// command-line flags override them:
//   --elf <path>      boot ELF (default: game/discA/SLPM_662.75 next to the executable)
//   --iso <path>      disc image the game reads sectors from (default: disc_a in dod.ini)
//   --disc A|B        which disc image to use when --iso is not given
//   --disc_mode dual|merged   two images (disc_a / disc_b) or one DVD9 image with both discs (disc_merged)
//   --window WxH      window size, --fullscreen 0|1
#include "ps2_runtime.h"
#include "dod_host_pacer.h"
#include <ps2_recompiled_functions.h> // [DoD] dodPreferParentEntries
#include "runtime/ee_scheduler.h"
#include "../lib/ps2_ipu.h"
#include "../lib/dod_input_settings.h"
#include "../lib/dod_audio_settings.h"
#include "../lib/gs/dod_pcsx2_gs.h"
#include "../lib/gs/dod_widescreen.h"

bool dodKzvuInstall(PS2Runtime &runtime, const char *mode); // [DoD] ../lib/vu/dod_kzvu.cpp

#include <raylib.h>

#if defined(_WIN32)
// raylib.h and windows.h both define names such as CloseWindow, Rectangle and DrawText; only the few Win32 calls
// below are needed, so windows.h comes in with the conflicting parts renamed.
#define CloseWindow Win32CloseWindow
#define Rectangle Win32Rectangle
#define ShowCursor Win32ShowCursor
#define DrawText Win32DrawText
#define DrawTextEx Win32DrawTextEx
#define LoadImage Win32LoadImage
#define PlaySound Win32PlaySound
#define NOMINMAX
#define NOGDI
#define NOUSER
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <timeapi.h>
#include <psapi.h> // [DoD] DOD_STALL_WATCH (page faults, working set)
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "winmm.lib") // [DoD] timeBeginPeriod for the DOD_PROF sampler
#undef CloseWindow
#undef Rectangle
#undef ShowCursor
#undef DrawText
#undef DrawTextEx
#undef LoadImage
#undef PlaySound
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>

namespace ps2x::iop
{
    void dodSetIopBreakpoints(const std::vector<uint32_t> &addresses); // ps2xIOP iop_emulator.cpp, [DoD] diagnostics
    extern void (*g_dodPadRumbleOutput)(int port, uint8_t large, uint8_t small);
    extern bool (*g_dodPadStateProvider)(int port, uint8_t *data32); // ps2xIOP iop_sio2.cpp
    extern bool (*g_dodPadPollStateProvider)(int port, uint8_t *data32);
}
void dodConfigureGamepad(bool enabled);
void dodRequestPadRumble(int port, uint8_t large, uint8_t small);
bool dodReadHostPadInputForPoll(uint8_t *data, size_t size); // ps2_pad.cpp, no public header dependency
bool dodPeekHostPadInput(uint8_t *data, size_t size);
extern uint64_t g_dodVif1OpcodeCounts[128]; // ps2_vif1_interpreter.cpp, [DoD] diagnostics
extern uint64_t g_dodXgkickPackets;         // vu/ps2_vu1_core.cpp
extern uint64_t g_dodXgkickBytes;
extern void (*g_dodGsInterruptHook)(uint32_t csrBit); // gs/gs_frontend.cpp
namespace ps2_stubs
{
    void dodConfigureIpuInit(uint32_t iqBase, uint32_t vqBase, uint32_t setD4ChcrEntry); // Kernel/Stubs/IPU.cpp
    void sceMpegDelete(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void dodInstallHdMovieHooks(PS2Runtime& runtime);
    void dodConfigureDiscs(const std::vector<std::filesystem::path> &images); // Kernel/Stubs/CD.cpp
    bool dodChangeDisc(size_t index);
    void dodCdSelfTest(const std::string &paths);
    void sceCdGetDiskType(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
}
static PS2Runtime *g_dodRuntime = nullptr;
extern std::atomic<int64_t> g_dodVBlankPeriodNs; // Kernel/EeScheduler.cpp: host time of a field
static PS2Runtime::RecompiledFunction g_dodDiscCheck = nullptr; // the game's own 0x2AC760
static PS2Runtime::RecompiledFunction g_dodAreaTask = nullptr;  // the game's own 0x1503B0 (area load task)
static int g_dodTestArea = -1;                                  // DOD_TEST_AREA: the first area load goes there
static unsigned g_dodTestAreaFrom = 0x67u;                      // DOD_TEST_AREA_FROM: which load is replaced
static int g_dodTestScene = -1;                                 // DOD_TEST_SCENE: the scene (room) set with it
static PS2Runtime::RecompiledFunction g_dodNewGame = nullptr;   // the game's own 0x360810 (new game start)
static int g_dodTestRoom = -1;                                  // DOD_TEST_ROOM: queued after the opening
static int g_dodTestRoomArea = 8;                               // DOD_TEST_ROOM_AREA: its area (8 = no movie, disc 1)
extern std::atomic<double> g_dodTestNewGameAt;                  // lib/ps2_pad.cpp: DOD_TEST_AUTOSTART
extern std::atomic<uint8_t *> g_dodTestRdram;                   // lib/ps2_pad.cpp: DOD_PAD_SCRIPT_FRAMES
static bool g_dodMergedDisc = false;                            // disc_mode = merged: one DVD9 image, no disc change

// [DoD] DOD_RNG_TRACE=1 (diagnostics, 2026-10-27; the player finds the enemies' choices still repetitive). The game
// draws its random numbers with VRNEXT from the VU0 R register, which 0x2B8590 seeds from its Mersenne Twister
// (0x1351C0) whenever R's mantissa is 0; the twister starts from a constant (0x1349A0: 5489 on first use, 1 from
// 0x177150, stored values from 0x1B5C10 / 0x1B71D0). The PS2 has one VU0, but here every guest thread keeps its own VU0
// registers (EeScheduler dodVu0Load), so a thread that starts fresh would restart the same sequence. Logs each R
// seeding and twister init (guest thread, its entry, the scene) and counts per guest thread the calls of the 21
// functions that draw with VRNEXT, summed up at each scene change and every 10 s.
namespace dodrng
{
    constexpr uint32_t kDrawFunctions[] = {0x2BA390u, 0x2BB7C0u, 0x2BC460u, 0x2BCE30u, 0x2BD3C0u, 0x2BD990u, 0x2BE810u,
                                           0x2C0190u, 0x2C1590u, 0x2C2280u, 0x2C24D0u, 0x2C6330u, 0x2C6D30u, 0x2C7740u,
                                           0x2C7E50u, 0x2C8120u, 0x318C70u, 0x321660u, 0x322060u, 0x335600u, 0x336480u};
    constexpr size_t kDrawCount = sizeof(kDrawFunctions) / sizeof(kDrawFunctions[0]);
    PS2Runtime::RecompiledFunction g_draw[kDrawCount] = {};
    PS2Runtime::RecompiledFunction g_seedCheck = nullptr;
    PS2Runtime::RecompiledFunction g_twisterInit = nullptr;
    std::atomic<uint64_t> g_calls[256];
    std::atomic<uint32_t> g_lastR[256];
    std::atomic<uint32_t> g_seeds[256];
    std::atomic<bool> g_on{false};
    std::atomic<unsigned> g_logged{0};

    uint32_t mantissa(const R5900Context *ctx)
    {
        return static_cast<uint32_t>(_mm_cvtsi128_si32(_mm_castps_si128(ctx->vu0_r))) & 0x007FFFFFu;
    }

    uint32_t scene(const uint8_t *rdram)
    {
        uint16_t value = 0;
        std::memcpy(&value, rdram + 0x007E1344u, 2);
        return value;
    }

    uint32_t threadEntry(PS2Runtime *rt, int id)
    {
        for (const EeThreadSnapshot &t : rt->eeScheduler().snapshot().threads)
            if (t.id == id)
                return t.entry;
        return 0u;
    }

    template <size_t I>
    void draw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        if (ctx->pc == kDrawFunctions[I]) // a call, not a resume inside the function
        {
            const size_t slot = static_cast<size_t>(rt->eeScheduler().currentThreadId()) & 255u;
            g_calls[slot].fetch_add(1u, std::memory_order_relaxed);
            g_lastR[slot].store(mantissa(ctx), std::memory_order_relaxed);
        }
        g_draw[I](rdram, ctx, rt);
    }

    void seedCheck(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        const bool zero = ctx->pc == 0x2B8590u && mantissa(ctx) == 0u;
        const int id = rt->eeScheduler().currentThreadId();
        const bool log = zero && g_logged.fetch_add(1u) < 96u;
        if (zero)
            g_seeds[static_cast<size_t>(id) & 255u].fetch_add(1u, std::memory_order_relaxed);
        if (log)
            std::fprintf(stderr, "[rng] R is 0 at 0x2B8590: thread %d (entry %08x) scene 0x%03X, caller %08x\n", id,
                         threadEntry(rt, id), scene(rdram), getRegU32(ctx, 31));
        g_seedCheck(rdram, ctx, rt);
        if (log)
            std::fprintf(stderr, "[rng]   R after: %06x (thread %d)\n", mantissa(ctx), rt->eeScheduler().currentThreadId());
    }

    void twisterInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        if (ctx->pc == 0x1349A0u && g_logged.fetch_add(1u) < 96u)
        {
            const int id = rt->eeScheduler().currentThreadId();
            std::fprintf(stderr, "[rng] twister init, seed %u: caller %08x thread %d (entry %08x) scene 0x%03X\n",
                         getRegU32(ctx, 4), getRegU32(ctx, 31), id, threadEntry(rt, id), scene(rdram));
        }
        g_twisterInit(rdram, ctx, rt);
    }

    template <size_t... I>
    size_t installDraw(PS2Runtime &runtime, std::index_sequence<I...>)
    {
        size_t hooked = 0;
        ((g_draw[I] = runtime.lookupFunction(kDrawFunctions[I]),
          hooked += (g_draw[I] && runtime.replaceFunction(kDrawFunctions[I], &draw<I>)) ? 1u : 0u),
         ...);
        return hooked;
    }

    void install(PS2Runtime &runtime)
    {
        const char *v = std::getenv("DOD_RNG_TRACE");
        if (!v || *v != '1')
            return;
        g_seedCheck = runtime.lookupFunction(0x2B8590u);
        g_twisterInit = runtime.lookupFunction(0x1349A0u);
        const bool seed = g_seedCheck && runtime.replaceFunction(0x2B8590u, &seedCheck);
        const bool twister = g_twisterInit && runtime.replaceFunction(0x1349A0u, &twisterInit);
        const size_t hooked = installDraw(runtime, std::make_index_sequence<kDrawCount>{});
        g_on.store(true, std::memory_order_release);
        std::fprintf(stderr, "[rng] trace on: seed check %s, twister init %s, %zu of %zu draw functions\n",
                     seed ? "hooked" : "MISSING", twister ? "hooked" : "MISSING", hooked, kDrawCount);
    }

    // Pad poll thread: draws per guest thread since the last summary, each thread's last R and its R seedings so far.
    void summary(const char *why, uint32_t sceneNow)
    {
        if (!g_on.load(std::memory_order_acquire))
            return;
        std::string line;
        for (size_t i = 0; i < 256u; ++i)
        {
            const uint64_t calls = g_calls[i].exchange(0u, std::memory_order_relaxed);
            const uint32_t seeds = g_seeds[i].load(std::memory_order_relaxed);
            if (!calls && !seeds)
                continue;
            char item[96];
            std::snprintf(item, sizeof(item), " thread %zu: %llu draws, R %06x, R==0 seen %u;", i,
                          static_cast<unsigned long long>(calls), g_lastR[i].load(std::memory_order_relaxed), seeds);
            line += item;
        }
        std::fprintf(stderr, "[rng] %s, scene 0x%03X:%s\n", why, sceneNow, line.empty() ? " no draws" : line.c_str());
    }
}

// [DoD] diagnostics: --watch. The host pages holding the watched EE RAM are made read-only; a vectored handler logs
// each write into the range (host RVA + the last guest functions entered), lets the instruction run single-stepped
// with the pages writable, then protects them again.
namespace dodwatch
{
    uint8_t *g_base = nullptr;
    uintptr_t g_lo = 0, g_hi = 0, g_pageLo = 0, g_pageHi = 0;
    std::atomic<int> g_hits{0};
    thread_local bool t_reprotect = false;
    constexpr int kMaxHits = 80;

    LONG CALLBACK handler(EXCEPTION_POINTERS *ep)
    {
        EXCEPTION_RECORD *rec = ep->ExceptionRecord;
        if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2 && rec->ExceptionInformation[0] == 1)
        {
            const uintptr_t addr = rec->ExceptionInformation[1];
            if (addr < g_pageLo || addr >= g_pageHi)
                return EXCEPTION_CONTINUE_SEARCH;
            DWORD old = 0;
            VirtualProtect(reinterpret_cast<void *>(g_pageLo), g_pageHi - g_pageLo, PAGE_READWRITE, &old);
            if (addr >= g_lo && addr < g_hi && g_hits.load() < kMaxHits)
            {
                ++g_hits;
                const uintptr_t module = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
                std::string last;
#if defined(DOD_FUNCTION_RING)
                for (uint32_t i = dod_ring::g_next - 4u; i != dod_ring::g_next; ++i)
                {
                    const char *name = dod_ring::g_names[i & (dod_ring::kSize - 1u)];
                    last += name ? name : "?";
                    last += ' ';
                }
#endif
                std::fprintf(stderr, "[watch] write to EE 0x%08X from rva 0x%llX after: %s\n",
                             static_cast<uint32_t>(addr - reinterpret_cast<uintptr_t>(g_base)),
                             static_cast<unsigned long long>(ep->ContextRecord->Rip - module), last.c_str());
            }
            ep->ContextRecord->EFlags |= 0x100u; // trap after this instruction
            t_reprotect = true;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (rec->ExceptionCode == EXCEPTION_SINGLE_STEP && t_reprotect)
        {
            t_reprotect = false;
            if (g_hits.load() < kMaxHits)
            {
                DWORD old = 0;
                VirtualProtect(reinterpret_cast<void *>(g_pageLo), g_pageHi - g_pageLo, PAGE_READONLY, &old);
            }
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

static void dodInstallWriteWatch(uint8_t *rdram, uint32_t guestAddress, uint32_t length, unsigned delaySeconds)
{
    if (!rdram || length == 0u)
        return;
    if (delaySeconds != 0u)
    {
        std::thread([=]()
                    {
                        std::this_thread::sleep_for(std::chrono::seconds(delaySeconds));
                        dodInstallWriteWatch(rdram, guestAddress, length, 0u); })
            .detach();
        return;
    }
    dodwatch::g_base = rdram;
    dodwatch::g_lo = reinterpret_cast<uintptr_t>(rdram) + (guestAddress & 0x1FFFFFFu);
    dodwatch::g_hi = dodwatch::g_lo + length;
    dodwatch::g_pageLo = dodwatch::g_lo & ~static_cast<uintptr_t>(0xFFF);
    dodwatch::g_pageHi = (dodwatch::g_hi + 0xFFF) & ~static_cast<uintptr_t>(0xFFF);
    AddVectoredExceptionHandler(1, &dodwatch::handler);
    DWORD old = 0;
    VirtualProtect(reinterpret_cast<void *>(dodwatch::g_pageLo), dodwatch::g_pageHi - dodwatch::g_pageLo, PAGE_READONLY, &old);
    std::fprintf(stderr, "[watch] EE 0x%08X..0x%08X\n", guestAddress, guestAddress + length);
}
extern uint64_t g_dodDmaStarts[64][2];  // ps2_memory.cpp
extern uint64_t g_dodIpuRegisterWrites;
extern uint64_t g_dodImageBytes;        // gs/gs_frontend.cpp
extern uint64_t g_dodImageNonZeroBytes;
extern std::atomic<uint64_t> g_dodHostFrames; // ps2_runtime.cpp host loop heartbeat

namespace
{
    namespace fs = std::filesystem;

#if defined(_WIN32)
    // --trace 1: native call stacks of the other threads, as RVAs of dod.exe. Recompiled guest functions call each
    // other as C++ functions, so the EE thread's stack is the guest call chain; tools/scripts/symbolize_trace.py
    // turns the RVAs into FUN_xxxxxxxx names with the linker map (dod.map). Nothing is allocated while a thread is
    // suspended (it could hold the heap lock).
    bool dodStacksWithModules = false; // [DoD] hang watchdog: also name frames outside dod.exe

    // [DoD] A crash in any thread (e.g. inside pcsx2gs.dll) used to end the process without a word: print the
    // exception and the faulting thread's call stack as module+offset (resolve with the module's linker map).
    void dodPrintModuleAddress(DWORD64 address)
    {
        HMODULE module = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(address), &module) && module)
        {
            char path[MAX_PATH] = {};
            GetModuleFileNameA(module, path, MAX_PATH);
            const char *slash = std::strrchr(path, '\\');
            std::snprintf(name, sizeof(name), "%s", slash ? slash + 1 : path);
        }
        std::fprintf(stderr, " %s+%llx", name, static_cast<unsigned long long>(address - reinterpret_cast<DWORD64>(module)));
    }

    LONG WINAPI dodCrashFilter(EXCEPTION_POINTERS *info)
    {
        static std::atomic<bool> reported{false};
        if (reported.exchange(true))
            return EXCEPTION_CONTINUE_SEARCH;
        const EXCEPTION_RECORD *record = info->ExceptionRecord;
        std::fprintf(stderr, "[crash] code=%08lX thread=%lu at", static_cast<unsigned long>(record->ExceptionCode),
                     static_cast<unsigned long>(GetCurrentThreadId()));
        dodPrintModuleAddress(reinterpret_cast<DWORD64>(record->ExceptionAddress));
        if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
            std::fprintf(stderr, " (%s %llx)", record->ExceptionInformation[0] ? "write" : "read",
                         static_cast<unsigned long long>(record->ExceptionInformation[1]));
        std::fprintf(stderr, "\n[crash] stack:");
        CONTEXT c = *info->ContextRecord;
        for (int i = 0; i < 48 && c.Rip != 0; ++i)
        {
            dodPrintModuleAddress(c.Rip);
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
            if (!fe)
            {
                c.Rip = *reinterpret_cast<DWORD64 *>(c.Rsp);
                c.Rsp += 8;
                continue;
            }
            PVOID handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fe, &c, &handlerData, &establisher, nullptr);
        }
        std::fprintf(stderr, "\n");
        std::fflush(stderr);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    void dumpThreadStacks()
    {
        const DWORD self = GetCurrentThreadId();
        const DWORD pid = GetCurrentProcessId();
        const auto base = reinterpret_cast<DWORD64>(GetModuleHandleW(nullptr));
        const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
        const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
        const DWORD64 end = base + nt->OptionalHeader.SizeOfImage;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE)
            return;
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
                continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!h)
                continue;
            DWORD64 frames[48];
            DWORD64 foreign[48];
            int count = 0;
            if (SuspendThread(h) != static_cast<DWORD>(-1))
            {
                CONTEXT c{};
                c.ContextFlags = CONTEXT_FULL;
                if (GetThreadContext(h, &c))
                {
                    for (int i = 0; i < 64 && count < 48 && c.Rip != 0; ++i)
                    {
                        if (c.Rip >= base && c.Rip < end)
                            frames[count] = c.Rip - base, foreign[count++] = 0;
                        else if (dodStacksWithModules)
                            frames[count] = 0, foreign[count++] = c.Rip; // [DoD] resolved after the thread resumes
                        DWORD64 imageBase = 0;
                        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
                        if (!fe)
                        {
                            c.Rip = *reinterpret_cast<DWORD64 *>(c.Rsp);
                            c.Rsp += 8;
                            continue;
                        }
                        PVOID handlerData = nullptr;
                        DWORD64 establisher = 0;
                        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fe, &c, &handlerData, &establisher, nullptr);
                    }
                }
                ResumeThread(h);
            }
            CloseHandle(h);
            if (count >= 3)
            {
                std::fprintf(stderr, "[stack] tid=%lu", static_cast<unsigned long>(te.th32ThreadID));
                for (int i = 0; i < count; ++i)
                {
                    if (!foreign[i])
                    {
                        std::fprintf(stderr, " %llx", static_cast<unsigned long long>(frames[i]));
                        continue;
                    }
                    HMODULE module = nullptr;
                    char name[MAX_PATH] = "?";
                    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                           reinterpret_cast<LPCSTR>(foreign[i]), &module) && module)
                    {
                        char path[MAX_PATH] = {};
                        GetModuleFileNameA(module, path, MAX_PATH);
                        const char *slash = std::strrchr(path, '\\');
                        std::snprintf(name, sizeof(name), "%s", slash ? slash + 1 : path);
                    }
                    std::fprintf(stderr, " %s+%llx", name,
                                 static_cast<unsigned long long>(foreign[i] - reinterpret_cast<DWORD64>(module)));
                }
                std::fprintf(stderr, "\n");
            }
        }
        CloseHandle(snap);
    }

    // [DoD] Hang watchdog, after the native stacks: the guest EE kernel as the scheduler last published it (threads,
    // and the semaphores / event flags someone waits on; from godhand-recomp's hang dump). The snapshot is copied under
    // its own lock, so this is safe while the game thread is stuck; the same seq in all three dumps means no guest
    // thread switch happened meanwhile (a loop inside one thread). DMA/timer registers are not read: readIORegister
    // has side effects and the register map is not safe to read from this thread.
    void dumpGuestKernel(const PS2Runtime &runtime)
    {
        static constexpr const char *kStatus[] = {"RUN", "READY", "WAIT", "WAIT+SUSP", "SUSP", "DORMANT"};
        static constexpr const char *kWait[] = {"", "sleep", "sema", "evflag", "vsync", "external", "mpeg"};
        const EeKernelSnapshot snapshot = runtime.eeScheduler().snapshot();
        std::fprintf(stderr, "[hang] guest EE kernel: seq %llu, cycle %llu, running thread %d, %zu threads, %zu semaphores, %zu event flags\n",
                     static_cast<unsigned long long>(snapshot.sequence), static_cast<unsigned long long>(snapshot.eeCycle),
                     snapshot.runningThreadId, snapshot.threads.size(), snapshot.semaphores.size(), snapshot.eventFlags.size());
        for (const EeThreadSnapshot &t : snapshot.threads)
        {
            const size_t status = static_cast<size_t>(t.status), wait = static_cast<size_t>(t.waitReason);
            std::fprintf(stderr, "[hang]   thread %2d %-9s %-8s id=%-3d prio=%d/%d pc=%08x ra=%08x sp=%08x entry=%08x depth=%u wakeups=%u suspends=%d\n",
                         t.id, status < sizeof(kStatus) / sizeof(kStatus[0]) ? kStatus[status] : "?",
                         wait < sizeof(kWait) / sizeof(kWait[0]) ? kWait[wait] : "?", t.waitId, t.currentPriority, t.initialPriority,
                         t.pc, t.ra, t.sp, t.entry, t.invocationDepth, t.wakeupCount, t.suspendCount);
        }
        for (const EeSemaphoreSnapshot &s : snapshot.semaphores)
            if (s.waiters != 0u)
                std::fprintf(stderr, "[hang]   sema %3d count %d/%d, %u waiting\n", s.id, s.count, s.maxCount, s.waiters);
        for (const EeEventFlagSnapshot &e : snapshot.eventFlags)
            if (e.waiters != 0u)
                std::fprintf(stderr, "[hang]   evflag %3d bits %08x, %u waiting\n", e.id, e.bits, e.waiters);
        std::fflush(stderr);
    }
#endif

    constexpr const char *kVersion = "0.1.0";

    std::string trim(const std::string &s)
    {
        const size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos)
            return {};
        const size_t e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    }

    std::map<std::string, std::string> readIni(const fs::path &path)
    {
        std::map<std::string, std::string> values;
        std::ifstream in(path);
        std::string line;
        for (bool first = true; std::getline(in, line); first = false)
        {
            if (first && line.rfind("\xEF\xBB\xBF", 0) == 0)
                line.erase(0, 3); // UTF-8 BOM (PowerShell 5 writes one)
            const std::string t = trim(line);
            if (t.empty() || t[0] == '#' || t[0] == ';')
                continue;
            const size_t eq = t.find('=');
            if (eq == std::string::npos || eq == 0)
                continue;
            values[trim(t.substr(0, eq))] = trim(t.substr(eq + 1));
        }
        return values;
    }

    fs::path executableDirectory(const char *argv0)
    {
        std::error_code ec;
        fs::path p = fs::absolute(fs::path(argv0), ec);
        if (ec)
            return fs::current_path();
        return p.parent_path();
    }

    fs::path resolveFrom(const fs::path &base, const std::string &value)
    {
        if (value.empty())
            return {};
        fs::path p(value);
        return (p.is_absolute() ? p : base / p).lexically_normal();
    }

    void setupTerminateLogger()
    {
#if defined(_WIN32)
        SetUnhandledExceptionFilter(dodCrashFilter);
#endif
        std::set_terminate([]()
                           {
                               std::cerr << "[terminate] unhandled exception" << std::endl;
                               if (const std::exception_ptr ep = std::current_exception())
                               {
                                   try { std::rethrow_exception(ep); }
                                   catch (const std::exception &e) { std::cerr << "[terminate] " << e.what() << std::endl; }
                                   catch (...) { std::cerr << "[terminate] non-std exception" << std::endl; }
                               }
                               std::abort(); });
    }
}

// [DoD] DOD_STALL_WATCH=1 (diagnostics, off by default): occasional ~0.1 s sound gaps where the IOP thread and even
// the sound card's callback stop at once (sweep 0x289 / 0x348, merged movie 57). A high-priority thread wakes every
// 5 ms and compares heartbeats: its own wake-up delay (the whole process not scheduled), the IOP thread's loop and
// step (ps2xIOP iop_subsystem.cpp), the SPU2 output and the sound card callback (ps2_iop_host.cpp), the EE cycle count
// and how long an EE-side caller has held the IOP lock. For each stall over 40 ms it prints one "[stall]" line with
// what stopped, the IOP step and this process's CPU time, page faults, working set and file reads during the stall,
// and the free RAM. Only this process is measured (no system-wide tracing).
namespace ps2x::iop
{
    extern std::atomic<uint64_t> g_dodIopBeatNs;
    extern std::atomic<uint32_t> g_dodIopStage;
    extern std::atomic<uint64_t> g_dodEeExecHoldSinceNs;
}
extern std::atomic<uint64_t> g_dodSpu2PushNs;
extern std::atomic<uint64_t> g_dodAudioCallbackNs;
extern std::atomic<uint64_t> g_dodIopEeCyclesTotal;
void dodAudioTraceNote(const char *text); // ps2_iop_host.cpp: an event in the audio trace (F9 events.txt)

namespace
{
    void dodStartStallWatch()
    {
#if defined(_WIN32)
        // Also on with the test launcher's audio trace, so a gap the tester hears is explained in F9's events.txt.
        const char *v = std::getenv("DOD_STALL_WATCH");
        const char *trace = std::getenv("DOD_AUDIO_TRACE");
        if (v && *v ? *v == '0' : !(trace && *trace))
            return;
        std::thread([]() {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            const auto now = []() {
                return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                 std::chrono::steady_clock::now().time_since_epoch())
                                                 .count());
            };
            struct Snap
            {
                uint64_t cpu100ns = 0, faults = 0, readBytes = 0, readOps = 0, workingSet = 0, availPhys = 0;
                uint32_t load = 0;
            };
            const auto snap = []() {
                Snap s;
                FILETIME c, e, k, u;
                if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u))
                    s.cpu100ns = ((static_cast<uint64_t>(k.dwHighDateTime) << 32) | k.dwLowDateTime) +
                                 ((static_cast<uint64_t>(u.dwHighDateTime) << 32) | u.dwLowDateTime);
                PROCESS_MEMORY_COUNTERS pmc{};
                if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
                {
                    s.faults = pmc.PageFaultCount;
                    s.workingSet = pmc.WorkingSetSize;
                }
                IO_COUNTERS io{};
                if (GetProcessIoCounters(GetCurrentProcess(), &io))
                {
                    s.readBytes = io.ReadTransferCount;
                    s.readOps = io.ReadOperationCount;
                }
                MEMORYSTATUSEX ms{};
                ms.dwLength = sizeof(ms);
                if (GlobalMemoryStatusEx(&ms))
                {
                    s.availPhys = ms.ullAvailPhys;
                    s.load = ms.dwMemoryLoad;
                }
                return s;
            };
            static const char *const kStages[] = {"waiting for EE cycles", "waiting for the IOP lock", "running the IOP",
                                                  "letting an EE-side caller in"};
            constexpr uint64_t kMs = 1000000ull;
            uint64_t last = now();
            uint64_t lastEeTotal = 0, lastEeMove = last;
            bool inStall = false;
            Snap s0;
            uint64_t stallStart = 0, maxLate = 0, maxIop = 0, maxPush = 0, maxCb = 0, maxEe = 0, maxHold = 0;
            uint32_t stageAtMax = 0;
            uint32_t lines = 0;
            for (;;)
            {
                Sleep(5);
                const uint64_t t = now();
                const uint64_t late = t - last > 5 * kMs ? t - last - 5 * kMs : 0;
                last = t;
                const auto age = [t](uint64_t beat) { return beat != 0 && t > beat ? t - beat : 0; };
                const uint64_t eeTotal = g_dodIopEeCyclesTotal.load(std::memory_order_relaxed);
                if (eeTotal != lastEeTotal)
                {
                    lastEeTotal = eeTotal;
                    lastEeMove = t;
                }
                const uint64_t iopAge = age(ps2x::iop::g_dodIopBeatNs.load(std::memory_order_relaxed));
                const uint64_t pushAge = age(g_dodSpu2PushNs.load(std::memory_order_relaxed));
                const uint64_t cbAge = age(g_dodAudioCallbackNs.load(std::memory_order_relaxed));
                const uint64_t eeAge = eeTotal != 0 ? t - lastEeMove : 0;
                const uint64_t hold = age(ps2x::iop::g_dodEeExecHoldSinceNs.load(std::memory_order_relaxed));
                const bool stalled = late > 30 * kMs || iopAge > 40 * kMs || pushAge > 60 * kMs || cbAge > 40 * kMs;
                if (stalled && !inStall)
                {
                    inStall = true;
                    s0 = snap();
                    stallStart = t - std::max({late, iopAge, pushAge, cbAge});
                    maxLate = maxIop = maxPush = maxCb = maxEe = maxHold = 0;
                }
                if (inStall)
                {
                    maxLate = std::max(maxLate, late);
                    if (iopAge >= maxIop)
                    {
                        maxIop = iopAge;
                        stageAtMax = ps2x::iop::g_dodIopStage.load(std::memory_order_relaxed);
                    }
                    maxPush = std::max(maxPush, pushAge);
                    maxCb = std::max(maxCb, cbAge);
                    maxEe = std::max(maxEe, eeAge);
                    maxHold = std::max(maxHold, hold);
                    if (!stalled)
                    {
                        inStall = false;
                        const Snap s1 = snap();
                        const uint64_t duration = t - stallStart;
                        if (duration > 40 * kMs && lines++ < 500u)
                        {
                            char text[512];
                            std::snprintf(text, sizeof(text),
                                          "stall %.0f ms: watcher late %.0f ms | IOP thread silent %.0f ms (%s) | SPU2 output silent "
                                          "%.0f ms | sound card callback silent %.0f ms | EE count still %.0f ms, EE held the IOP "
                                          "lock %.0f ms | process CPU +%.0f ms, page faults +%llu, working set %.0f -> %.0f MB, "
                                          "file reads +%.1f MB (%llu), free RAM %.0f MB (load %u%%)",
                                          duration / 1e6, maxLate / 1e6, maxIop / 1e6, kStages[stageAtMax & 3u], maxPush / 1e6,
                                          maxCb / 1e6, maxEe / 1e6, maxHold / 1e6, (s1.cpu100ns - s0.cpu100ns) / 1e4,
                                          static_cast<unsigned long long>(s1.faults - s0.faults), s0.workingSet / 1048576.0,
                                          s1.workingSet / 1048576.0, (s1.readBytes - s0.readBytes) / 1048576.0,
                                          static_cast<unsigned long long>(s1.readOps - s0.readOps), s1.availPhys / 1048576.0,
                                          s1.load);
                            std::fprintf(stderr, "[stall] %s\n", text + 6);
                            dodAudioTraceNote(text);
                        }
                    }
                }
            }
        }).detach();
        std::fprintf(stderr, "[stall] watcher on (DOD_STALL_WATCH)\n");
#endif
    }
}

// [DoD] RUNTIME_PATCHES row 123: dispatch-table slots that the jump-table scan gave to a small entry_ fragment
// although the enclosing function can be entered at the same address (a case of its entry switch). A loop entered
// at a fragment hopped from fragment to fragment through the scheduler; the enclosing function runs the same guest
// instructions on gotos. The list comes from tools/scripts/prefer_parent_entries.py; an entry whose slot no longer
// matches its address (code regenerated without running the script again) is skipped. DOD_PREFER_PARENT=0: off.
static void dodPreferParentEntries()
{
    const char *v = std::getenv("DOD_PREFER_PARENT");
    if (v && v[0] == '0')
        return;
    extern const uint32_t g_ps2RecompiledFunctionTableBase;
    extern const uint32_t g_ps2RecompiledFunctionTableSlotCount;
    uint32_t applied = 0u, skipped = 0u;
#define DOD_PREFER_PARENT(slot, address, function)                                                                    \
    if ((slot) < g_ps2RecompiledFunctionTableSlotCount &&                                                             \
        g_ps2RecompiledFunctionTableBase + 4u * (slot) == (address) && g_ps2RecompiledFunctionTable[(slot)] != nullptr) \
    {                                                                                                                  \
        g_ps2RecompiledFunctionTable[(slot)] = &function;                                                              \
        ++applied;                                                                                                     \
    }                                                                                                                  \
    else                                                                                                               \
        ++skipped;
#include "dod_prefer_parent_entries.inc"
#undef DOD_PREFER_PARENT
    std::fprintf(stderr, "[dod] dispatch: %u fragment entries go to their enclosing function (%u skipped)\n", applied,
                 skipped);
}

int main(int argc, char *argv[])
{
    setupTerminateLogger();
    dodStartStallWatch();
    dodPreferParentEntries();
    // Logs must survive the process being killed (the launcher or a test stopping a hung game).
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::cout << std::unitbuf;
    try
    {
        const fs::path exeDir = executableDirectory(argv[0]);
        std::map<std::string, std::string> ini = readIni(exeDir / "dod.ini");

        for (int i = 1; i + 1 < argc; i += 2)
        {
            std::string key = argv[i];
            if (key.rfind("--", 0) != 0)
            {
                std::cerr << "unknown argument: " << key << std::endl;
                return 2;
            }
            ini[key.substr(2)] = argv[i + 1];
        }

        dod_input::KeyboardBindings keyboard;
        std::string keyboardError;
        if (!dod_input::load(ini, keyboard, keyboardError))
            std::cerr << "[dod] invalid keyboard settings; using defaults: " << keyboardError << std::endl;
        dodConfigureKeyboardInput(keyboard);
        std::string stickError;
        dodConfigureKeyboardSticks(dod_input::loadSticks(ini, keyboard, stickError));
        if (!stickError.empty())
            std::cerr << "[dod] keyboard stick keys: " << stickError << std::endl;
        dodConfigureGamepad(!ini.count("rumble") || ini["rumble"] != "0");
        ps2x::iop::g_dodPadRumbleOutput = dodRequestPadRumble;

        const fs::path elf = resolveFrom(exeDir, ini.count("elf") ? ini["elf"] : "game/discA/SLPM_662.75");
        // disc_a / disc_b: the images of disc 1 and disc 2. The game boots from disc 1 (from disc 2 with disc = B, or
        // when there is no disc 1) and the other image goes in when the game asks for it (see 0x2AC760 below).
        // iso = <image> boots that image instead.
        const fs::path discImages[2] = {resolveFrom(exeDir, ini.count("disc_a") ? ini["disc_a"] : ""),
                                        resolveFrom(exeDir, ini.count("disc_b") ? ini["disc_b"] : "")};
        const auto haveDisc = [&](int i)
        {
            std::error_code e;
            return !discImages[i].empty() && fs::exists(discImages[i], e);
        };
        std::string disc = ini.count("disc") ? ini["disc"] : ((haveDisc(0) || !haveDisc(1)) ? "A" : "B");
        std::transform(disc.begin(), disc.end(), disc.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        // disc_mode = merged, disc_merged = <image>: one DVD9 image holding both discs (Onimusha_JP_DVD9.iso: disc 1's
        // files on layer 0, disc 2's own files in layer 1's file system, which the CD index reads too). Its ELF is ours
        // with 0x2AC760 (the disc check before an area loads) returning at once; merged mode does the same below.
        // Without disc_mode, merged is chosen when only disc_merged exists.
        const fs::path mergedImage = resolveFrom(exeDir, ini.count("disc_merged") ? ini["disc_merged"] : "");
        std::error_code mergedError;
        const bool haveMerged = !mergedImage.empty() && fs::exists(mergedImage, mergedError);
        std::string discMode = ini.count("disc_mode") ? ini["disc_mode"] : ((haveMerged && !haveDisc(0) && !haveDisc(1)) ? "merged" : "dual");
        std::transform(discMode.begin(), discMode.end(), discMode.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        g_dodMergedDisc = discMode == "merged";
        if (g_dodMergedDisc)
            disc = "A";
        const fs::path iso = ini.count("iso") ? resolveFrom(exeDir, ini["iso"])
                                              : (g_dodMergedDisc ? mergedImage : discImages[disc == "B" ? 1 : 0]);
        const fs::path saveRoot = resolveFrom(exeDir, ini.count("savedata") ? ini["savedata"] : "savedata");

        std::cout << "[dod] host v" << kVersion << " elf=" << elf.string() << " iso=" << iso.string()
                  << " disc=" << disc << " mode=" << (g_dodMergedDisc ? "merged" : "dual") << std::endl;
        if (!fs::exists(elf))
        {
            std::cerr << "[dod] boot ELF not found: " << elf.string() << std::endl;
            return 1;
        }
        if (iso.empty() || !fs::exists(iso))
        {
            std::cerr << "[dod] disc image not found: " << (iso.empty() ? std::string("(not set)") : iso.string())
                      << (g_dodMergedDisc ? " (set disc_merged in dod.ini or pass --iso)" : " (set disc_a / disc_b in dod.ini or pass --iso)")
                      << std::endl;
            return 1;
        }

        // The game reads its data sectors from the disc image; the boot files beside the ELF stay the CD root.
        PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
        paths.cdImage = iso;
        PS2Runtime::setIoPaths(paths);
        if (g_dodMergedDisc)
            ps2_stubs::dodConfigureDiscs({});
        else
            ps2_stubs::dodConfigureDiscs({haveDisc(0) ? discImages[0] : fs::path(), haveDisc(1) ? discImages[1] : fs::path()});
        if (const char *selfTest = std::getenv("DOD_CD_SELFTEST"))
            ps2_stubs::dodCdSelfTest(selfTest);

        PS2Runtime runtime;
        const std::string title = std::string("鬼武者:夢之曙光 | PC v") + kVersion;
        if (!runtime.initialize(title.c_str()))
        {
            std::cerr << "[dod] failed to initialize the runtime" << std::endl;
            return 1;
        }

        // Embedded branding supplies the live GLFW window icon as well as the
        // executable's Explorer icon. No working-directory image dependency.
#if defined(_WIN32)
        HMODULE brandingModule = GetModuleHandleW(nullptr);
        HRSRC brandingResource = FindResourceW(brandingModule, reinterpret_cast<LPCWSTR>(102), reinterpret_cast<LPCWSTR>(10));
        if (brandingResource)
        {
            HGLOBAL loaded = LoadResource(brandingModule, brandingResource);
            const unsigned char* bytes = static_cast<const unsigned char*>(LockResource(loaded));
            const DWORD count = SizeofResource(brandingModule, brandingResource);
            if (bytes && count)
            {
                Image iconImage = LoadImageFromMemory(".png", bytes, static_cast<int>(count));
                if (iconImage.data) { SetWindowIcon(iconImage); UnloadImage(iconImage); std::cout << "[dod] embedded window icon applied" << std::endl; }
            }
        }
#endif

        // Device-level gain covers the SPU2/CG stream and all host sounds without
        // changing emulated mixer state, audio timing, or the system-wide volume.
        const int volumePercent = dod_audio::percent(ini.count("volume") ? ini["volume"] : "100");
        if (IsAudioDeviceReady())
        {
            SetMasterVolume(static_cast<float>(volumePercent) / 100.0f);
            std::cout << "[audio] master volume=" << volumePercent << "% applied=" << GetMasterVolume() << std::endl;
        }

        // VU1: vu1 = microvu (default; PCSX2's microVU through kzvu), interp (PCSX2's VU interpreter) or builtin
        // (PS2Recomp's VU1Interpreter).
        dodKzvuInstall(runtime, ini.count("vu1") ? ini["vu1"].c_str() : "microvu");

        // Esc is the keyboard's circle (cancel) button; it must not close the window as raylib's default exit key.
        SetExitKey(KEY_NULL);

        int width = 0, height = 0;
        if (ini.count("window"))
        {
            if (dod_input::windowSize(ini["window"], width, height))
                SetWindowSize(width, height);
            else
                std::cerr << "[dod] invalid window size; keeping default: " << ini["window"] << std::endl;
        }
        const bool fullscreen = ini.count("fullscreen") && ini["fullscreen"] == "1";
        if (fullscreen)
            ToggleBorderlessWindowed();
        // Game fields keep their existing independent 60 Hz scheduler. Neither
        // presentation path uses VSync, including with legacy lock/gs_vsync INIs.
        g_dodVBlankPeriodNs.store(16666667);
        ClearWindowState(FLAG_VSYNC_HINT);
        // Extra host/presentation-loop cap only. The guest fields, SPU2 and CG
        // retain their native clocks when this cap is disabled (as in Oni3).
        // Missing/invalid settings preserve the previous 60 FPS host default.
        const bool extraLimit60 = !ini.count("fps_limit") || ini["fps_limit"] != "0";
        g_dodLimitHost60 = extraLimit60;
        SetTargetFPS(0); // One deadline pacer in the host loop; no raylib double wait.
        std::cout << "[dod] extra host fps limit=" << (extraLimit60 ? 60 : 0)
                  << "; native game fields=60 Hz; vsync off; deadline pacer" << std::endl;

        // GS renderer (dod_pcsx2_gs.h): renderer = pcsx2 (default; PCSX2's hardware GS, pcsx2gs.dll) or cpu (the
        // port's own raster); gs_scale = internal resolution 1..8; gs_vsync 0 off / 1 on / 2 mailbox;
        // gs_options = PCSX2 image options ("fxaa=1;texture_filtering=2;...", see pcsx2gs_api.h).
        {
            // [DoD] DOD_RENDERER (tests): the renderer without editing dod.ini, e.g. the CPU raster against PCSX2's GS.
            const char *rendererEnv = std::getenv("DOD_RENDERER");
            const std::string renderer = rendererEnv && *rendererEnv ? rendererEnv
                                         : ini.count("renderer")   ? ini["renderer"]
                                                                   : "pcsx2";
            std::string gsOptions = ini.count("gs_options") ? ini["gs_options"] : "";
            const char* aspectEnv = std::getenv("DOD_ASPECT");
            const std::string aspect = aspectEnv && *aspectEnv ? aspectEnv : ini.count("aspect") ? ini["aspect"] : "4:3";
            dodwide::enabled = renderer == "pcsx2" && aspect == "16:9";
            // [DoD] DOD_GS_OPTIONS (tests): more PCSX2 options after dod.ini's, e.g. its draw dumps
            // ("dump_dir=...;dump_frame=3900;dump_rt=1"); DOD_GS_SCALE: the internal resolution.
            if (const char *extra = std::getenv("DOD_GS_OPTIONS"); extra && *extra)
                gsOptions += (gsOptions.empty() ? "" : ";") + std::string(extra);
            gsOptions += std::string(";widescreen=") + (dodwide::enabled ? "1" : "0");
            // [DoD] fullscreen = 1 is DXGI exclusive fullscreen in the desktop's own resolution and refresh rate (the
            // display settings never change; Alt+Tab returns to it). fullscreen_mode = borderless keeps the old
            // borderless window. The window itself is borderless and monitor-sized in both cases.
            const bool exclusive = fullscreen && !(ini.count("fullscreen_mode") && ini["fullscreen_mode"] == "borderless");
            if (exclusive)
                gsOptions += ";exclusive_fullscreen=1";
            std::cout << "[dod] fullscreen " << (!fullscreen ? "off" : exclusive ? "exclusive (desktop mode)" : "borderless") << std::endl;
            std::fprintf(stderr, "[widescreen] %s; camera position unchanged\n", dodwide::enabled ? "16:9 Hor+" : "4:3 original");
            // gs_scale follows the display resolution unless dod.ini sets a number: the smallest multiple of the
            // PS2's 640x448 at least as tall as the 4:3 picture the window (or the monitor, borderless) shows, so the
            // 3D image is never upscaled: 720p -> 2, 960 or 1080 -> 3, 1440 -> 4, 2160 -> 5.
            int scale = ini.count("gs_scale") && ini["gs_scale"] != "auto" ? std::atoi(ini["gs_scale"].c_str()) : 0;
            if (const char *s = std::getenv("DOD_GS_SCALE"); s && *s)
                scale = std::atoi(s);
            if (scale <= 0)
            {
                int outW = width > 0 ? width : GetScreenWidth();
                int outH = height > 0 ? height : GetScreenHeight();
                if (fullscreen)
                {
                    const int monitor = GetCurrentMonitor();
                    outW = GetMonitorWidth(monitor);
                    outH = GetMonitorHeight(monitor);
                }
                const int pictureH = std::max(1, std::min(outH, dodwide::enabled ? outW * 9 / 16 : outW * 3 / 4));
                // [DoD] ssaa = 1.5 / 2: supersampling, the picture rendered that much taller than shown and filtered
                // down at presentation. Every PS2 draw (blending, feedback, depth) stays in one single-sample target,
                // unlike the experimental msaa.
                const double ssaa = ini.count("ssaa") ? std::clamp(std::atof(ini["ssaa"].c_str()), 1.0, 2.0) : 1.0;
                scale = std::clamp(static_cast<int>(std::ceil(pictureH * ssaa / 448.0 - 1e-9)), 1, 8);
                if (ssaa > 1.0)
                    std::cout << "[dod] ssaa " << ssaa << "x" << std::endl;
                std::cout << "[dod] render scale " << scale << " for a " << outW << "x" << outH << " display" << std::endl;
            }
            dodpcsx2::configure(renderer.c_str(), scale, 0, gsOptions.c_str());
        }

        if (!runtime.loadELF(elf.string()))
        {
            std::cerr << "[dod] failed to load ELF: " << elf.string() << std::endl;
            return 1;
        }

        // 0x110C08 prints a finished line through SIF to the IOP's TTY on hardware (CRI's ADX/DVCI messages such
        // as "E0092911:sceCdSearchFile fail." come through it); print it on the host instead.
        runtime.replaceFunction(0x00110C08u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
                                {
                                    const uint32_t address = getRegU32(ctx, 4);
                                    std::string text;
                                    for (uint32_t i = 0; i < 512u; ++i)
                                    {
                                        const char c = static_cast<char>(rdram[(address + i) & PS2_RAM_MASK]);
                                        if (c == '\0')
                                            break;
                                        text += c;
                                    }
                                    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
                                        text.pop_back();
                                    std::fprintf(stderr, "[ee tty] %s\n", text.c_str());
                                    setReturnS32(ctx, static_cast<int32_t>(text.size()));
                                    ctx->pc = getRegU32(ctx, 31);
                                });

        // The game's own pad stack (SIO2MAN/DBCMAN/DS2O_D) runs on the emulated IOP; its SIO2 sees a DualShock 2
        // in port 1 driven by the keyboard / host gamepad mapping of PSPadBackend.
        ps2x::iop::g_dodPadStateProvider = [](int port, uint8_t *data32)
        {
            return port == 0 && dodPeekHostPadInput(data32, 32u);
        };
        // [DoD] Area / room tests: log each change of the current scene (0x7E1344) and area (0x7E1348), checked on
        // every pad poll (once a field).
        static uint8_t *s_testRdram = nullptr;
        if (std::getenv("DOD_TEST_ROOM") || std::getenv("DOD_TEST_AREA"))
            s_testRdram = runtime.memory().getRDRAM();
        g_dodTestRdram.store(runtime.memory().getRDRAM(), std::memory_order_release);
        ps2x::iop::g_dodPadPollStateProvider = [](int port, uint8_t *data32)
        {
            // [DoD] HD texture room prefetch: tell the GS when the scene number changes (it changes when the game
            // starts loading the room, so that room's replacements load and upload during its loading screen).
            if (port == 0)
            {
                static uint16_t s_prefetchScene = 0xFFFFu;
                if (const uint8_t *rdram = g_dodTestRdram.load(std::memory_order_acquire))
                {
                    uint16_t scene = 0;
                    std::memcpy(&scene, rdram + 0x007E1344u, 2);
                    static unsigned s_rngPolls = 0u;
                    if (scene != s_prefetchScene)
                        dodrng::summary("scene change", s_prefetchScene), s_rngPolls = 0u;
                    else if (++s_rngPolls >= 600u)
                        dodrng::summary("10 s", scene), s_rngPolls = 0u;
                    if (scene != s_prefetchScene)
                    {
                        s_prefetchScene = scene;
                        dodpcsx2::prefetchScene(scene);
                    }
                }
            }
            if (port == 0 && s_testRdram)
            {
                static uint32_t last = 0xFFFFFFFFu, logged = 0u;
                uint16_t scene = 0;
                std::memcpy(&scene, s_testRdram + 0x007E1344u, 2);
                const uint32_t now = (static_cast<uint32_t>(scene) << 8) | s_testRdram[0x007E1348u];
                if (now != last && logged < 400u)
                {
                    ++logged;
                    last = now;
                    std::fprintf(stderr, "[dod-test] scene now 0x%03X area %u\n", scene, s_testRdram[0x007E1348u]);
                }
            }
            return port == 0 && dodReadHostPadInputForPoll(data32, 32u);
        };

        // Disc change. 0x2AC760 (the disc check before an area loads) takes the area's discs from the area table
        // (halfword flags at 0x4DF7D6 + area * 8, bits 4-5 = disc 1 / disc 2). When the disc in the drive (the game's
        // disc index at $gp - 0x7A50, set by its boot-file search) is not one of them it shows "insert disc N" and
        // waits for a tray change (sceCdTrayReq), then searches for that disc's boot file. Exchange the image first,
        // as a player would, and let the game's own check run.
        dodrng::install(runtime);
        g_dodDiscCheck = runtime.lookupFunction(0x002AC760u);
        if (g_dodDiscCheck)
            runtime.replaceFunction(0x002AC760u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
                                    {
                                        if (g_dodMergedDisc && ctx->pc == 0x002AC760u)
                                        {
                                            // Merged DVD9: every area's files are on the one disc. As the merged
                                            // release's ELF (jr $ra; nop at 0x2AC760): no check, no disc change.
                                            // The only caller (0x2ACD90) does not use the return value.
                                            static unsigned logged = 0u;
                                            if (logged++ < 64u)
                                            {
                                                const uint32_t area = getRegU32(ctx, 4);
                                                uint16_t flags = 0;
                                                std::memcpy(&flags, rdram + ((0x004DF7D6u + area * 8u) & PS2_RAM_MASK), 2);
                                                std::cerr << "[dod] area " << area << " (disc mask " << ((flags >> 4) & 3u)
                                                          << "): merged image, no disc check" << std::endl;
                                            }
                                            ctx->pc = getRegU32(ctx, 31);
                                            return;
                                        }
                                        if (ctx->pc == 0x002AC760u) // a call, not a resume inside the function
                                        {
                                            const uint32_t area = getRegU32(ctx, 4);
                                            uint16_t flags = 0;
                                            uint32_t current = 0;
                                            std::memcpy(&flags, rdram + ((0x004DF7D6u + area * 8u) & PS2_RAM_MASK), 2);
                                            std::memcpy(&current, rdram + ((getRegU32(ctx, 28) - 0x7A50u) & PS2_RAM_MASK), 4);
                                            const uint32_t need = (flags >> 4) & 3u;
                                            if (need != 0u && current < 2u && (need & (1u << current)) == 0u)
                                            {
                                                const size_t want = (need & 1u) ? 0u : 1u;
                                                std::cerr << "[dod] area " << area << " is on disc " << (want + 1)
                                                          << " (disc " << (current + 1) << " is in)" << std::endl;
                                                if (!ps2_stubs::dodChangeDisc(want))
                                                    std::cerr << "[dod] no image for disc " << (want + 1) << "; set disc_"
                                                              << (want ? 'b' : 'a') << " in dod.ini" << std::endl;
                                            }
                                        }
                                        g_dodDiscCheck(rdram, ctx, rt);
                                    });
        // [DoD] Automated area test (tools/scripts/area_sweep.ps1): DOD_TEST_AREA=<area> sends the first area load
        // after a new game to that area instead of area 0. 0x1503B0 is the area load task: byte 0x7E1340 is its step
        // (1 = load now), byte 0x7E1348 the area it loads (read by the special-area check 0x2AE080, the load 0x2ACC30 -
        // disc check, display width, files - and the area's entry). Areas: the table at 0x4DF7D0 (8 bytes each, flags
        // halfword at +6: bits 4-5 = disc 1 / 2); area files IMAGE/<area >> 4>/<area hex>.
        if (const char *testArea = std::getenv("DOD_TEST_AREA"))
        {
            g_dodTestArea = static_cast<int>(std::strtol(testArea, nullptr, 0));
            if (const char *from = std::getenv("DOD_TEST_AREA_FROM"))
                g_dodTestAreaFrom = static_cast<unsigned>(std::strtoul(from, nullptr, 0)) & 0x7Fu;
            // Halfword 0x7E1344 is the scene (room) the task enters after the area's movie or event; the rooms are
            // listed in the table at 0x67A760 (24 bytes each, scene first).
            if (const char *scene = std::getenv("DOD_TEST_SCENE"))
                g_dodTestScene = static_cast<int>(std::strtol(scene, nullptr, 0));
            if (std::strcmp(testArea, "log") == 0)
                g_dodTestArea = -1; // only log the loads
            g_dodAreaTask = runtime.lookupFunction(0x001503B0u);
            if (g_dodAreaTask && g_dodTestArea < 0x80)
                runtime.replaceFunction(0x001503B0u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
                                        {
                                            // A new game plays the prologue's event area 0x66 and then the prologue's
                                            // first playable area 0x67 (the town, enemies, the soul tutorial): that load
                                            // (DOD_TEST_AREA_FROM) goes to the test area instead, in normal play.
                                            if (ctx->pc == 0x001503B0u && rdram[0x007E1340u] == 1u)
                                            {
                                                const unsigned from = rdram[0x007E1348u];
                                                if (g_dodTestArea >= 0 && from == g_dodTestAreaFrom)
                                                {
                                                    rdram[0x007E1348u] = static_cast<uint8_t>(g_dodTestArea);
                                                    if (g_dodTestScene >= 0)
                                                    {
                                                        const uint16_t scene = static_cast<uint16_t>(g_dodTestScene);
                                                        std::memcpy(rdram + 0x007E1344u, &scene, 2);
                                                    }
                                                    std::cerr << "[dod-test] area load " << from << " -> test area " << g_dodTestArea
                                                              << " scene 0x" << std::hex << g_dodTestScene << std::dec << std::endl;
                                                    g_dodTestArea = -1;
                                                }
                                                else
                                                {
                                                    uint16_t scene = 0, nextScene = 0;
                                                    std::memcpy(&scene, rdram + 0x007E1344u, 2);
                                                    std::memcpy(&nextScene, rdram + 0x008BAC4Cu, 2);
                                                    std::cerr << "[dod-test] area load " << from << " scene 0x" << std::hex << scene
                                                              << " (next scene 0x" << nextScene << " area 0x"
                                                              << static_cast<unsigned>(rdram[0x008BAC50u]) << ")" << std::dec << std::endl;
                                                }
                                            }
                                            g_dodAreaTask(rdram, ctx, rt);
                                        });
        }

        // [DoD] Automated room test (tools/scripts/room_sweep.ps1): DOD_TEST_ROOM=<scene> makes a new game go to that room
        // after the opening movie, through the game's own scene queue. 0x360810 starts a new game: it sets the first
        // (scene, area) - (0xFF7, 0x66), the opening movie and the prologue event - in the area load task at 0x7E1340
        // (+4 scene, +8 area) and queues (0xFF7, 0x67), (0xFF1, 1), (0x181, 0) with 0x14FB50 (+0x50 scenes, +0x60
        // areas, +0x68 count). The queue is replaced by the test room; the prologue event takes it as its next scene.
        // The area of a queued scene picks the movie or event played on the way: area 8 (flags 0x18) and 55 (0x28)
        // have bit 3 set, which 0x2ACC30 treats as "no movie" (disc 1 / disc 2). Rooms: the table at 0x67A760.
        const char *room = std::getenv("DOD_TEST_ROOM");
        if (room || std::getenv("DOD_TEST_AUTOSTART"))
        {
            g_dodTestRoom = room ? static_cast<int>(std::strtol(room, nullptr, 0)) : -1;
            if (const char *area = std::getenv("DOD_TEST_ROOM_AREA"))
                g_dodTestRoomArea = static_cast<int>(std::strtol(area, nullptr, 0));
            g_dodNewGame = runtime.lookupFunction(0x00360810u);
            if (g_dodNewGame && g_dodTestRoom < 0x1000)
                runtime.replaceFunction(0x00360810u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
                                        {
                                            const bool call = ctx->pc == 0x00360810u;
                                            g_dodNewGame(rdram, ctx, rt);
                                            if (!call)
                                                return;
                                            if (g_dodTestNewGameAt.load() < 0.0)
                                            {
                                                g_dodTestNewGameAt.store(GetTime());
                                                std::cerr << "[dod-test] new game started at " << GetTime() << " s" << std::endl;
                                            }
                                            if (g_dodTestRoom < 0)
                                                return;
                                            // The new game starts in the test room (as the game does after the prologue
                                            // with (0x181, 0)), nothing queued after it. Queuing it after the opening
                                            // does not work: the prologue's first scene is itself the playable tutorial,
                                            // which only takes the next queued scene when it is cleared.
                                            const uint16_t scene = static_cast<uint16_t>(g_dodTestRoom);
                                            std::memcpy(rdram + 0x007E1344u, &scene, 2);
                                            rdram[0x007E1348u] = static_cast<uint8_t>(g_dodTestRoomArea);
                                            rdram[0x007E13A8u] = 0u; // queue count
                                            std::cerr << "[dod-test] new game: starts in room 0x" << std::hex << g_dodTestRoom
                                                      << " area " << std::dec << g_dodTestRoomArea << std::endl;
                                            g_dodTestRoom = -1;
                                        });
        }

        // [DoD] DOD_TEST_LOADLOG=1 (frozen-room diagnosis): log the game's resource load requests (0x153060: disc
        // context, file id) and its AFS opens through CRI ADXF (0x22F500: partition, file id; partition 1 = IMAGE.AFS,
        // 12052 files). A room whose resource cannot be opened is retried forever (0x1537C0); repeats are counted.
        if (std::getenv("DOD_TEST_LOADLOG"))
        {
            static PS2Runtime::RecompiledFunction s_loadRequest = runtime.lookupFunction(0x00153060u);
            static PS2Runtime::RecompiledFunction s_afsOpen = runtime.lookupFunction(0x0022F500u);
            struct Repeat
            {
                uint64_t key = ~0ull;
                uint32_t count = 0u;
                uint32_t lines = 0u;
                void log(const char *what, uint32_t a, uint32_t b)
                {
                    const uint64_t k = (static_cast<uint64_t>(a) << 32) | b;
                    if (k == key)
                    {
                        if ((++count & (count - 1u)) == 0u) // 2, 4, 8, ... repeats
                            std::fprintf(stderr, "[dod-load] %s %u %#x repeated %u times\n", what, a, b, count);
                        return;
                    }
                    key = k;
                    count = 1u;
                    if (lines++ < 3000u)
                        std::fprintf(stderr, "[dod-load] %s %u %#x\n", what, a, b);
                }
            };
            if (s_loadRequest)
                runtime.replaceFunction(0x00153060u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
                                        {
                                            static Repeat repeat;
                                            if (ctx->pc == 0x00153060u)
                                                repeat.log("request ctx", getRegU32(ctx, 4) & 0xFFFFu, getRegU32(ctx, 5));
                                            s_loadRequest(rdram, ctx, rt);
                                        });
            if (s_afsOpen)
                runtime.replaceFunction(0x0022F500u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
                                        {
                                            static Repeat repeat;
                                            if (ctx->pc == 0x0022F500u)
                                                repeat.log("afs open partition", getRegU32(ctx, 4), getRegU32(ctx, 5));
                                            s_afsOpen(rdram, ctx, rt);
                                        });
        }

        // [DoD] DOD_LAYER_LOG=1 (VIF1 chain overlap, rooms 0x2CD / 0x8CD / 0x8AA): the frame's VIF1 DMA list has 33
        // layers; 0x1868D0(list, start, end, layer) links the packet chain start..end (end = its last tag) after the
        // layer's tail tag (list + 0x420 + layer * 4) via 0x178CF0, 0x186910 the same without moving the tail. Each
        // call walks the packet's tags from start: a cnt whose data runs past `end`, a jump out of it or an unexpected
        // tag id is printed with the caller (ra), layer, sizes and the packet heap pointer 0x81A510. Every 4096th call
        // and the first 64 calls of a layer are listed too.
        if (std::getenv("DOD_LAYER_LOG"))
        {
            static PS2Runtime::RecompiledFunction s_layerAdd = runtime.lookupFunction(0x001868D0u);
            static PS2Runtime::RecompiledFunction s_layerLink = runtime.lookupFunction(0x00186910u);
            struct LayerCheck
            {
                static uint64_t tagAt(const uint8_t *rdram, uint32_t address)
                {
                    uint64_t tag;
                    std::memcpy(&tag, rdram + (address & 0x01FFFFF0u), sizeof(tag));
                    return tag;
                }
                static void check(const uint8_t *rdram, R5900Context *ctx, const char *which)
                {
                    static uint64_t calls = 0u;
                    static uint32_t bad = 0u, perLayer[64] = {};
                    const uint32_t start = getRegU32(ctx, 5), end = getRegU32(ctx, 6), layer = getRegU32(ctx, 7);
                    const uint32_t ra = getRegU32(ctx, 31);
                    uint32_t heap;
                    std::memcpy(&heap, rdram + 0x0081A510u, sizeof(heap));
                    ++calls;
                    // The DMA walk from start to end as the DMAC does it (PCSX2 hwDmacSrcChainWithStack), with the call
                    // stack of the packet itself (entered by a NEXT from the layer tail, so at depth 0).
                    uint32_t p = start, steps = 0u, depth = 0u, maxDepth = 0u, stack[2] = {0u, 0u};
                    const char *problem = nullptr;
                    uint64_t badTag = 0u;
                    uint32_t badAt = 0u;
                    while (!(p == end && depth == 0u) && steps++ < 100000u)
                    {
                        const uint64_t tag = tagAt(rdram, p);
                        const uint32_t id = static_cast<uint32_t>(tag >> 28) & 7u;
                        const uint32_t qwc = static_cast<uint32_t>(tag) & 0xFFFFu;
                        const uint32_t addr = static_cast<uint32_t>(tag >> 32) & 0x7FFFFFF0u;
                        const uint32_t after = p + 16u * (qwc + 1u);
                        bool stop = false;
                        switch (id)
                        {
                        case 1: // cnt
                            if (depth == 0u && end > p && after > end)
                            {
                                problem = "cnt runs past the packet end";
                                stop = true;
                            }
                            p = after;
                            break;
                        case 2: // next
                            p = addr;
                            break;
                        case 3: // ref
                        case 4: // refs
                            p += 16u;
                            break;
                        case 5: // call
                            if (depth == 2u)
                            {
                                problem = "third nested CALL (address stack overflow)";
                                stop = true;
                                break;
                            }
                            stack[depth++] = after;
                            maxDepth = std::max(maxDepth, depth);
                            p = addr;
                            break;
                        case 6: // ret
                            if (depth == 0u)
                            {
                                problem = "RET with an empty stack (ends the chain)";
                                stop = true;
                                break;
                            }
                            p = stack[--depth];
                            break;
                        default:
                            problem = "end / refe / unknown tag";
                            stop = true;
                            break;
                        }
                        if (stop)
                        {
                            badTag = tag;
                            badAt = p;
                            break;
                        }
                    }
                    if (!problem && !(p == end && depth == 0u))
                        problem = "walk did not reach the end";
                    const bool list = calls % 4096u == 1u || (layer < 64u && perLayer[layer]++ < 64u && layer == 14u);
                    if ((problem && bad++ < 400u) || list)
                        std::fprintf(stderr,
                                     "[dod-layer] %s %s ra %06X layer %u start %08X end %08X (%u qw) heap %08X call depth %u%s tag@%08X %016llX\n",
                                     which, problem ? problem : "ok", ra - 8u, layer, start, end, (end - start) / 16u, heap,
                                     maxDepth, problem ? " BAD" : "", badAt, static_cast<unsigned long long>(badTag));
                }
            };
            if (s_layerAdd)
                runtime.replaceFunction(0x001868D0u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
                                        {
                                            if (ctx->pc == 0x001868D0u)
                                                LayerCheck::check(rdram, ctx, "add");
                                            s_layerAdd(rdram, ctx, rt);
                                        });
            if (s_layerLink)
                runtime.replaceFunction(0x00186910u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
                                        {
                                            if (ctx->pc == 0x00186910u)
                                                LayerCheck::check(rdram, ctx, "link");
                                            s_layerLink(rdram, ctx, rt);
                                        });
        }

        // After the tray change the check calls libcdvd's sceCdGetDiskType (S-command 3) at 0x1205D8, which is not among
        // the stubbed functions (the stub list's "sceCdGetDiskType" is its wrapper at 0x120670): its RPC to the IOP's
        // cdvdfsv never returns. Answer it like the wrapper's stub.
        runtime.replaceFunction(0x001205D8u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
                                {
                                    ctx->pc = getRegU32(ctx, 31);
                                    ps2_stubs::sceCdGetDiskType(rdram, ctx, rt);
                                });

        // libipu's sceIpuInit tables in SLPM_662.75 (intra/non-intra IQ at 0x3FD690, VQ CLUT at 0x3FD6E0).
        ps2_stubs::dodConfigureIpuInit(0x003FD690u, 0x003FD6E0u, 0u);

        // DoD's Sofdec cleanup at 0x21A5E0 tail-calls the native libmpeg
        // destructor. Pair it with the existing HLE Create so its host decoder
        // and DMA input ownership end at the same point as the guest session.
        // [DoD] Only with the old MPEG HLE (DOD_IPU=legacy and code generated with the sceMpeg* stubs); the default
        // build runs the game's own libmpeg on the AC5 IPU (work/recomp/dod_native_ipu.toml).
        if (const char *ipu = std::getenv("DOD_IPU"); ipu && std::strcmp(ipu, "legacy") == 0)
            runtime.replaceFunction(0x0010B890u, ps2_stubs::sceMpegDelete);
        else
            ps2_stubs::dodInstallHdMovieHooks(runtime);

        // The GS SIGNAL/FINISH interrupt goes to the EE's INTC GS handlers at once (unless IMR masks it): the game
        // decodes each IPU macroblock row from its GS SIGNAL handler while the GS waits on the next SIGNAL.
        g_dodRuntime = &runtime;
        g_dodGsInterruptHook = [](uint32_t csrBit)
        {
            const uint64_t imr = g_dodRuntime->memory().gs().imr;
            const uint64_t maskBit = csrBit == 0x1u ? (1ull << 8) : (1ull << 9);
            if ((imr & maskBit) != 0u)
                return;
            g_dodRuntime->eeScheduler().runIntcHandlersNow(0u); // INTC cause 0: GS
        };

        // loadELF points the memory card root beside the ELF; keep saves in our own folder instead.
        paths = PS2Runtime::getIoPaths();
        paths.mcRoot = saveRoot / "mc0";
        std::error_code ec;
        fs::create_directories(paths.mcRoot, ec);
        PS2Runtime::setIoPaths(paths);

        // --trace 1: every 2 s print where the guest is (last scheduler checkpoint) and the IOP's progress.
        std::atomic<bool> traceStop{false};
        std::thread tracer;
        std::vector<std::tuple<uint32_t, uint32_t, bool>> peeks, iopeeks;
        const auto parsePeeks = [](const std::string &list, std::vector<std::tuple<uint32_t, uint32_t, bool>> &out)
        {
            for (size_t pos = 0; pos <= list.size();)
            {
                size_t comma = list.find(',', pos);
                std::string item = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                pos = comma == std::string::npos ? list.size() + 1 : comma + 1;
                bool deref = !item.empty() && item[0] == '*';
                if (deref)
                    item.erase(0, 1);
                unsigned a = 0, n = 16;
                if (std::sscanf(item.c_str(), "%x:%x", &a, &n) >= 1)
                    out.emplace_back(a, std::min(n, 256u), deref);
            }
        };
        if (ini.count("iopeek"))
            parsePeeks(ini["iopeek"], iopeeks);
        if (ini.count("iopbp"))
        {
            // --iopbp addr[,addr...] (hex IOP addresses): log the registers whenever the IOP executes one of them
            std::vector<uint32_t> breakpoints;
            std::vector<std::tuple<uint32_t, uint32_t, bool>> parsed;
            parsePeeks(ini["iopbp"], parsed);
            for (const auto &[address, unusedLength, unusedDeref] : parsed)
                breakpoints.push_back(address);
            ps2x::iop::dodSetIopBreakpoints(breakpoints);
        }
        if (ini.count("peek"))
        {
            std::string list = ini["peek"];
            for (size_t pos = 0; pos <= list.size();)
            {
                size_t comma = list.find(',', pos);
                std::string item = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                pos = comma == std::string::npos ? list.size() + 1 : comma + 1;
                bool deref = !item.empty() && item[0] == '*';
                if (deref)
                    item.erase(0, 1);
                unsigned a = 0, n = 16;
                if (std::sscanf(item.c_str(), "%x:%x", &a, &n) >= 1)
                    peeks.emplace_back(a, std::min(n, 256u), deref);
            }
        }
        if (ini.count("watch"))
        {
            // --watch addr:len[:seconds] (hex, hex, decimal): log host code (RVA) and recent guest functions for
            // writes to EE RAM there, starting after the given delay
            unsigned a = 0, n = 16, delay = 0;
            if (std::sscanf(ini["watch"].c_str(), "%x:%x:%u", &a, &n, &delay) >= 1)
                dodInstallWriteWatch(runtime.memory().getRDRAM(), a, n, delay);
        }
        if (ini.count("trace") && ini["trace"] == "1")
        {
            const bool stacks = ini.count("stacks") && ini["stacks"] == "1";
            const bool gsdump = ini.count("gsdump") && ini["gsdump"] == "1";
            runtime.gs().setDebugHistoryPaused(false);
            tracer = std::thread([&runtime, &traceStop, &peeks, &iopeeks, stacks, gsdump]()
                                 {
                                     size_t diagnosticsShown = 0;
                                     // GS events accumulated from the 512-entry debug history, drained every 100 ms
                                     std::map<std::string, unsigned> gsDraws, gsTransfers;
                                     unsigned gsKinds[5] = {};
                                     std::vector<GSDebugHistoryEntry> gsLastDraws;
                                     const auto drainGs = [&]()
                                     {
                                         const auto history = runtime.gs().getDebugHistory();
                                         runtime.gs().clearDebugHistory();
                                         char key[96];
                                         static int s_badRegReports = 0;
                                         for (size_t idx = 0; idx < history.size() && s_badRegReports < 3; ++idx)
                                         {
                                             const GSDebugHistoryEntry &h = history[idx];
                                             if (h.kind != GSDebugEventKind::Register || (h.reg != 0x59 && h.reg != 0x5B && h.reg != 0x5F))
                                                 continue;
                                             ++s_badRegReports; // [DoD] who writes the pseudo DISPFB/BGCOLOR registers?
                                             std::fprintf(stderr, "[gs] write to reg 0x%02X value=%016llX; preceding events:\n", h.reg,
                                                          (unsigned long long)h.regValue);
                                             for (size_t k = idx > 24 ? idx - 24 : 0; k <= idx; ++k)
                                             {
                                                 const GSDebugHistoryEntry &e = history[k];
                                                 if (e.kind == GSDebugEventKind::GifTag)
                                                     std::fprintf(stderr, "[gs]     giftag size=%u nloop=%u flg=%u nreg=%u\n", e.gifSizeBytes, e.gifNloop, e.gifFlg, e.gifNreg);
                                                 else if (e.kind == GSDebugEventKind::Register)
                                                     std::fprintf(stderr, "[gs]     reg 0x%02X = %016llX\n", e.reg, (unsigned long long)e.regValue);
                                                 else
                                                     std::fprintf(stderr, "[gs]     event kind %u\n", static_cast<unsigned>(e.kind));
                                             }
                                             break;
                                         }
                                         for (const GSDebugHistoryEntry &h : history)
                                         {
                                             if (static_cast<unsigned>(h.kind) < 5u)
                                                 ++gsKinds[static_cast<unsigned>(h.kind)];
                                             if (h.kind == GSDebugEventKind::Draw)
                                             {
                                                 std::snprintf(key, sizeof(key), "fbp=%X psm=%u prim=%d tme=%d", h.frame.fbp, h.frame.psm,
                                                               static_cast<int>(h.prim.type), h.prim.tme ? 1 : 0);
                                                 ++gsDraws[key];
                                                 gsLastDraws.push_back(h);
                                                 if (gsLastDraws.size() > 8u)
                                                     gsLastDraws.erase(gsLastDraws.begin());
                                             }
                                             else if (h.kind == GSDebugEventKind::Transfer)
                                             {
                                                 std::snprintf(key, sizeof(key), "dbp=%X dbw=%u dpsm=%X", h.bitbltbuf.dbp, h.bitbltbuf.dbw, h.bitbltbuf.dpsm);
                                                 ++gsTransfers[key];
                                             }
                                         }
                                     };
                                     for (int tick = 1; !traceStop.load(); ++tick)
                                     {
                                         for (int i = 0; i < 20 && !traceStop.load(); ++i)
                                         {
                                             std::this_thread::sleep_for(std::chrono::milliseconds(100));
                                             drainGs();
                                         }
                                         const auto iop = runtime.iopDebugSnapshot();
                                         std::fprintf(stderr, "[trace %3ds] ee pc=%08X ra=%08X sp=%08X | iop cycles=%llu insns=%llu modules=%u threads=%u rpc=%u\n",
                                                      tick * 2, runtime.m_debugPc.load(), runtime.m_debugRa.load(), runtime.m_debugSp.load(),
                                                      static_cast<unsigned long long>(iop.emulatorCycles),
                                                      static_cast<unsigned long long>(iop.emulatorInstructions), iop.emulatorLoadedModules,
                                                      iop.emulatorThreads, iop.emulatorRpcServers);
                                         // --peek addr:len[,addr:len...] (hex): dump guest memory every tick; "*addr:len"
                                         // dumps len bytes at the address stored in the word at addr.
                                         for (const auto &[peekAddr, peekLen, deref] : peeks)
                                         {
                                             const uint8_t *ram = runtime.memory().getRDRAM();
                                             uint32_t a = peekAddr;
                                             if (deref)
                                             {
                                                 uint32_t p = 0;
                                                 std::memcpy(&p, ram + (a & PS2_RAM_MASK), 4);
                                                 a = p;
                                             }
                                             std::string hex;
                                             char b[4];
                                             for (uint32_t i = 0; i < peekLen; ++i)
                                             {
                                                 std::snprintf(b, sizeof(b), "%02X", ram[(a + i) & PS2_RAM_MASK]);
                                                 hex += b;
                                                 if ((i & 3u) == 3u)
                                                     hex += ' ';
                                             }
                                             std::fprintf(stderr, "[peek %3ds] %s%08X @%08X: %s\n", tick * 2, deref ? "*" : "", peekAddr, a, hex.c_str());
                                         }
                                         // --iopeek addr:len[,...] (hex): the same for IOP memory
                                         for (const auto &[peekAddr, peekLen, deref] : iopeeks)
                                         {
                                             (void)deref;
                                             std::vector<uint8_t> bytes(peekLen);
                                             std::string hex;
                                             if (runtime.readIopMemory(peekAddr, bytes.data(), bytes.size()))
                                             {
                                                 char b[4];
                                                 for (uint32_t i = 0; i < peekLen; ++i)
                                                 {
                                                     std::snprintf(b, sizeof(b), "%02X", bytes[i]);
                                                     hex += b;
                                                     if ((i & 3u) == 3u)
                                                         hex += ' ';
                                                 }
                                             }
                                             std::fprintf(stderr, "[iopeek %3ds] %08X: %s\n", tick * 2, peekAddr, hex.c_str());
                                         }
                                         if (tick % 5 == 1)
                                         {
                                             static const char *const statusNames[] = {"running", "ready", "waiting", "wait-susp", "suspended", "dormant"};
                                             static const char *const waitNames[] = {"-", "sleep", "sema", "evflag", "vsync", "external", "mpeg"};
                                             const EeKernelSnapshot ee = runtime.eeScheduler().snapshot();
                                             for (const EeThreadSnapshot &t : ee.threads)
                                                 std::fprintf(stderr, "[trace]   ee thread %d %s/%s waitId=%d pc=%08X ra=%08X entry=%08X prio=%d wakeups=%u\n",
                                                              t.id, statusNames[static_cast<int>(t.status)], waitNames[static_cast<int>(t.waitReason)],
                                                              t.waitId, t.pc, t.ra, t.entry, t.currentPriority, t.wakeupCount);
                                             for (const EeSemaphoreSnapshot &s : ee.semaphores)
                                                 std::fprintf(stderr, "[trace]   ee sema %d count=%d/%d waiters=%u\n", s.id, s.count, s.maxCount, s.waiters);
                                             for (const std::string &t : iop.threads)
                                                 std::fprintf(stderr, "[trace]   iop %s\n", t.c_str());
                                             {
                                                 // GS: display registers and what reached the GS since the last report
                                                 const GSRegisters &gr = runtime.memory().gs();
                                                 std::fprintf(stderr, "[gs] pmode=%llX smode2=%llX dispfb1=%llX display1=%llX dispfb2=%llX display2=%llX bgcolor=%llX csr=%llX\n",
                                                              (unsigned long long)gr.pmode, (unsigned long long)gr.smode2, (unsigned long long)gr.dispfb1,
                                                              (unsigned long long)gr.display1, (unsigned long long)gr.dispfb2, (unsigned long long)gr.display2,
                                                              (unsigned long long)gr.bgcolor, (unsigned long long)gr.csr.load());
                                                 if (gsdump)
                                                 {
                                                     // --gsdump 1: frame buffers at FBP 0x00/0x40/0x80 (512x224 PSMCT32) as PPM, plus pixel stats
                                                     std::error_code dirError;
                                                     fs::create_directories("logs/gsdump", dirError);
                                                     for (const uint32_t fbp : {0x00u, 0x40u, 0x80u})
                                                     {
                                                         std::vector<uint8_t> rgb(512u * 224u * 3u);
                                                         unsigned nonBlack = 0, nonZeroAlpha = 0;
                                                         for (uint32_t y = 0; y < 224u; ++y)
                                                             for (uint32_t x = 0; x < 512u; ++x)
                                                             {
                                                                 const uint32_t p = runtime.gs().ReadVram(0u, fbp * 32u, 8u, x, y);
                                                                 uint8_t *d = rgb.data() + (y * 512u + x) * 3u;
                                                                 d[0] = static_cast<uint8_t>(p);
                                                                 d[1] = static_cast<uint8_t>(p >> 8);
                                                                 d[2] = static_cast<uint8_t>(p >> 16);
                                                                 nonBlack += (p & 0xFFFFFFu) != 0u;
                                                                 nonZeroAlpha += (p >> 24) != 0u;
                                                             }
                                                         char name[64];
                                                         std::snprintf(name, sizeof(name), "logs/gsdump/fbp%02X_%03ds.ppm", fbp, tick * 2);
                                                         if (FILE *f = std::fopen(name, "wb"))
                                                         {
                                                             std::fprintf(f, "P6\n512 224\n255\n");
                                                             std::fwrite(rgb.data(), 1, rgb.size(), f);
                                                             std::fclose(f);
                                                         }
                                                         std::fprintf(stderr, "[gs]   fbp %02X: nonblack=%u alpha!=0 %u -> %s\n", fbp, nonBlack, nonZeroAlpha, name);
                                                     }
                                                 }
                                                 drainGs();
                                                 std::fprintf(stderr, "[gs] giftag=%u reg=%u draw=%u transfer=%u present=%u (last 10 s)\n",
                                                              gsKinds[0], gsKinds[1], gsKinds[2], gsKinds[3], gsKinds[4]);
                                                 {
                                                     static uint64_t lastVif[128] = {};
                                                     static uint64_t lastKicks = 0, lastKickBytes = 0;
                                                     std::string vifLine;
                                                     for (unsigned op = 0; op < 128u; ++op)
                                                     {
                                                         const uint64_t n = g_dodVif1OpcodeCounts[op] - lastVif[op];
                                                         lastVif[op] = g_dodVif1OpcodeCounts[op];
                                                         if (n != 0u)
                                                         {
                                                             char item[32];
                                                             std::snprintf(item, sizeof(item), " %02X:%llu", op, static_cast<unsigned long long>(n));
                                                             vifLine += item;
                                                         }
                                                     }
                                                     std::fprintf(stderr, "[gs]   vif1 opcodes:%s | xgkick packets=%llu bytes=%llu\n", vifLine.c_str(),
                                                                  static_cast<unsigned long long>(g_dodXgkickPackets - lastKicks),
                                                                  static_cast<unsigned long long>(g_dodXgkickBytes - lastKickBytes));
                                                     lastKicks = g_dodXgkickPackets;
                                                     lastKickBytes = g_dodXgkickBytes;
                                                     {
                                                         static uint64_t lastDma[64][2] = {};
                                                         static uint64_t lastIpu = 0;
                                                         std::string dmaLine;
                                                         for (unsigned ch = 0; ch < 64u; ++ch)
                                                             for (unsigned dir = 0; dir < 2u; ++dir)
                                                             {
                                                                 const uint64_t n = g_dodDmaStarts[ch][dir] - lastDma[ch][dir];
                                                                 lastDma[ch][dir] = g_dodDmaStarts[ch][dir];
                                                                 if (n != 0u)
                                                                 {
                                                                     char item[48];
                                                                     std::snprintf(item, sizeof(item), " %08X/dir%u:%llu", 0x10000000u | (ch << 10), dir,
                                                                                   static_cast<unsigned long long>(n));
                                                                     dmaLine += item;
                                                                 }
                                                             }
                                                         std::fprintf(stderr, "[gs]   dma starts:%s | ipu reg writes=%llu\n", dmaLine.c_str(),
                                                                      static_cast<unsigned long long>(g_dodIpuRegisterWrites - lastIpu));
                                                         lastIpu = g_dodIpuRegisterWrites;
                                                     }
                                                     {
                                                         const ps2_ipu::Stats ipu = ps2_ipu::stats();
                                                         std::fprintf(stderr, "[gs]   ipu commands=%llu macroblocks=%llu in=%llu out=%llu unsupported=%llu (totals)\n",
                                                                      (unsigned long long)ipu.commands, (unsigned long long)ipu.macroblocks,
                                                                      (unsigned long long)ipu.inputBytes, (unsigned long long)ipu.outputBytes,
                                                                      (unsigned long long)ipu.errors);
                                                     }
                                                     static uint64_t lastImage = 0, lastImageNonZero = 0;
                                                     std::fprintf(stderr, "[gs]   image data bytes=%llu nonzero=%llu\n",
                                                                  static_cast<unsigned long long>(g_dodImageBytes - lastImage),
                                                                  static_cast<unsigned long long>(g_dodImageNonZeroBytes - lastImageNonZero));
                                                     lastImage = g_dodImageBytes;
                                                     lastImageNonZero = g_dodImageNonZeroBytes;
                                                 }
                                                 for (const auto &[k, n] : gsDraws)
                                                     std::fprintf(stderr, "[gs]   draws %6u x %s\n", n, k.c_str());
                                                 for (const auto &[k, n] : gsTransfers)
                                                     std::fprintf(stderr, "[gs]   xfers %6u x %s\n", n, k.c_str());
                                                 for (const GSDebugHistoryEntry &h : gsLastDraws)
                                                     std::fprintf(stderr, "[gs]   last draw prim=%d tme=%d fbp=%X fbw=%u psm=%u verts=%u x=%.0f..%.0f y=%.0f..%.0f a=%u..%u tex0.tbp=%X\n",
                                                                  static_cast<int>(h.prim.type), h.prim.tme ? 1 : 0, h.frame.fbp, h.frame.fbw, h.frame.psm,
                                                                  h.vertexCount, h.xMin, h.xMax, h.yMin, h.yMax, h.aMin, h.aMax, h.tex0.tbp0);
                                                 gsDraws.clear();
                                                 gsTransfers.clear();
                                                 gsLastDraws.clear();
                                                 std::fill(std::begin(gsKinds), std::end(gsKinds), 0u);
                                             }
#if defined(DOD_FUNCTION_RING)
                                             {
                                                 // last guest function entries, consecutive repeats collapsed
                                                 const uint32_t end = dod_ring::g_next;
                                                 const uint32_t count = std::min<uint32_t>(end, 400u);
                                                 std::string line;
                                                 const char *prev = nullptr;
                                                 int repeat = 0;
                                                 for (uint32_t i = end - count; i != end; ++i)
                                                 {
                                                     const char *n = dod_ring::g_names[i & (dod_ring::kSize - 1u)];
                                                     if (n == prev)
                                                     {
                                                         ++repeat;
                                                         continue;
                                                     }
                                                     if (prev && repeat)
                                                         line += "x" + std::to_string(repeat + 1);
                                                     line += line.empty() ? "" : " ";
                                                     line += n ? n : "?";
                                                     prev = n;
                                                     repeat = 0;
                                                 }
                                                 std::fprintf(stderr, "[ring] %u calls total; last %u: %s\n", end, count, line.c_str());
                                                 // and how often each function appears in the whole ring
                                                 std::map<std::string, int> histogram;
                                                 const uint32_t all = std::min<uint32_t>(end, dod_ring::kSize);
                                                 for (uint32_t i = end - all; i != end; ++i)
                                                 {
                                                     const char *n = dod_ring::g_names[i & (dod_ring::kSize - 1u)];
                                                     ++histogram[n ? n : "?"];
                                                 }
                                                 std::vector<std::pair<int, std::string>> ranked;
                                                 for (const auto &[name, n] : histogram)
                                                     ranked.emplace_back(n, name);
                                                 std::sort(ranked.rbegin(), ranked.rend());
                                                 std::string top;
                                                 for (size_t i = 0; i < ranked.size() && i < 60u; ++i)
                                                     top += ranked[i].second + "=" + std::to_string(ranked[i].first) + " ";
                                                 std::fprintf(stderr, "[ring-hist] last %u calls: %s\n", all, top.c_str());
                                             }
#endif
#if defined(_WIN32)
                                             // a burst of samples: the EE thread only shows guest frames while it runs guest
                                             // code. --stacks 1 only: suspending the EE thread while it unwinds a scheduler
                                             // transfer exception can deadlock this tracer.
                                             for (int sample = 0; stacks && sample < 200 && !traceStop.load(); ++sample)
                                             {
                                                 dumpThreadStacks();
                                                 std::this_thread::sleep_for(std::chrono::milliseconds(2));
                                             }
#endif
                                         }
                                         for (; diagnosticsShown < iop.diagnostics.size(); ++diagnosticsShown)
                                             std::fprintf(stderr, "[trace] iop: %s\n", iop.diagnostics[diagnosticsShown].c_str());
                                     } });
        }

#if defined(_WIN32)
        // [DoD] Hang watchdog (DOD_HANG_DUMP=1): when the host loop (window thread) or the guest vsync (game thread,
        // counted by PCSX2's GS presents) stops for 4 s, print every thread's native stack three times, 1 s apart,
        // with frames outside dod.exe as module+offset, then the guest EE threads (dumpGuestKernel). Nothing else runs
        // while it waits. DOD_HANG_DUMP_AT=<s> also dumps once <s> seconds after this point, stuck or not (to see the
        // state of a run that a test script would end by its time limit, and to try the dump itself).
        std::thread hangWatchdog;
        if (const char *hang = std::getenv("DOD_HANG_DUMP"); hang && *hang == '1')
        {
            const char *at = std::getenv("DOD_HANG_DUMP_AT");
            const double forceAfter = at && *at ? std::atof(at) : 0.0;
            hangWatchdog = std::thread([&traceStop, &runtime, forceAfter]()
                                       {
                uint64_t lastHost = g_dodHostFrames.load(), lastGuest = dodpcsx2::presents();
                const auto started = std::chrono::steady_clock::now();
                auto hostSince = started, guestSince = hostSince;
                bool forcePending = forceAfter > 0.0;
                int dumps = 0;
                while (!traceStop.load() && dumps < 3)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    const auto now = std::chrono::steady_clock::now();
                    const uint64_t host = g_dodHostFrames.load(), guest = dodpcsx2::presents();
                    if (host != lastHost)
                        lastHost = host, hostSince = now;
                    if (guest != lastGuest)
                        lastGuest = guest, guestSince = now;
                    const bool hostStuck = now - hostSince > std::chrono::seconds(4);
                    const bool guestStuck = guest != 0u && now - guestSince > std::chrono::seconds(4);
                    const bool forced = forcePending && now - started >= std::chrono::duration<double>(forceAfter);
                    if (!hostStuck && !guestStuck && !forced)
                        continue;
                    forcePending = forcePending && !forced;
                    std::fprintf(stderr, "[hang] host loop %s (frame %llu), guest vsync %s (field %llu); dump %d%s\n",
                                 hostStuck ? "STOPPED" : "running", static_cast<unsigned long long>(host),
                                 guestStuck ? "STOPPED" : "running", static_cast<unsigned long long>(guest), dumps + 1,
                                 forced ? " (DOD_HANG_DUMP_AT)" : "");
                    dodStacksWithModules = true;
                    dumpThreadStacks();
                    dumpGuestKernel(runtime);
                    ++dumps;
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                } });
        }
#endif
#if defined(_WIN32)
        // [DoD] Sampling profiler (DOD_PROF=<start s>:<seconds>): about every millisecond, each other thread's
        // instruction pointer (leaf) and the dod.exe return addresses on its stack (inclusive, each once per sample).
        // Printed per thread as module+offset; tools/scripts/diag/prof_symbolize.py names them with the linker maps.
        std::thread profiler;
        if (const char *prof = std::getenv("DOD_PROF"))
        {
            const double startSec = std::atof(prof);
            const char *colon = std::strchr(prof, ':');
            const double lengthSec = colon ? std::atof(colon + 1) : 10.0;
            profiler = std::thread([&traceStop, startSec, lengthSec]()
                                   {
                timeBeginPeriod(1);
                const auto t0 = std::chrono::steady_clock::now();
                while (!traceStop.load() && std::chrono::steady_clock::now() - t0 < std::chrono::duration<double>(startSec))
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
                const auto base = reinterpret_cast<DWORD64>(GetModuleHandleW(nullptr));
                const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
                const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
                const DWORD64 end = base + nt->OptionalHeader.SizeOfImage;
                std::vector<std::pair<DWORD, HANDLE>> threads;
                HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                THREADENTRY32 te{};
                te.dwSize = sizeof(te);
                for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
                    if (te.th32OwnerProcessID == pid && te.th32ThreadID != self)
                        if (HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID))
                            threads.emplace_back(te.th32ThreadID, h);
                CloseHandle(snap);
                std::map<std::pair<DWORD, DWORD64>, uint32_t> leaf, inclusive, externalCaller;
                std::map<DWORD, uint32_t> totals;
                const auto stopAt = std::chrono::steady_clock::now() + std::chrono::duration<double>(lengthSec);
                DWORD64 frames[16];
                while (!traceStop.load() && std::chrono::steady_clock::now() < stopAt)
                {
                    for (auto &[tid, h] : threads)
                    {
                        int count = 0;
                        DWORD64 rip = 0;
                        if (SuspendThread(h) == static_cast<DWORD>(-1))
                            continue;
                        CONTEXT c{};
                        c.ContextFlags = CONTEXT_FULL;
                        if (GetThreadContext(h, &c))
                        {
                            rip = c.Rip;
                            for (int i = 0; i < 40 && count < 16 && c.Rip != 0; ++i)
                            {
                                if (c.Rip >= base && c.Rip < end)
                                    frames[count++] = c.Rip - base;
                                DWORD64 imageBase = 0;
                                PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
                                if (!fe)
                                {
                                    // JIT code (microVU) has no unwind data: treat it as a leaf and pop the return
                                    // address, as the crash handler does.
                                    if (i > 2)
                                        break;
                                    c.Rip = *reinterpret_cast<DWORD64 *>(c.Rsp);
                                    c.Rsp += 8;
                                    continue;
                                }
                                PVOID handlerData = nullptr;
                                DWORD64 establisher = 0;
                                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fe, &c, &handlerData, &establisher, nullptr);
                            }
                        }
                        ResumeThread(h);
                        if (!rip)
                            continue;
                        ++totals[tid];
                        ++leaf[{tid, rip}];
                        if ((rip < base || rip >= end) && count > 0)
                            ++externalCaller[{tid, frames[0]}]; // first dod.exe frame above a leaf in another module
                        for (int i = 0; i < count; ++i)
                        {
                            bool seen = false;
                            for (int j = 0; j < i; ++j)
                                seen |= frames[j] == frames[i];
                            if (!seen)
                                ++inclusive[{tid, frames[i]}];
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                for (auto &[tid, total] : totals)
                {
                    if (total < 50u)
                        continue;
                    std::fprintf(stderr, "[prof] thread %lu samples %u\n", static_cast<unsigned long>(tid), total);
                    {
                        // Leaf time by module ("jit" = no module: microVU's generated code).
                        std::map<std::string, uint32_t> byModule;
                        for (auto &[key, n] : leaf)
                        {
                            if (key.first != tid)
                                continue;
                            HMODULE module = nullptr;
                            char path[MAX_PATH] = "jit";
                            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                   reinterpret_cast<LPCSTR>(key.second), &module) && module)
                                GetModuleFileNameA(module, path, MAX_PATH);
                            const char *slash = std::strrchr(path, '\\');
                            byModule[slash ? slash + 1 : path] += n;
                        }
                        for (auto &[name, n] : byModule)
                            if (n * 200u >= total)
                                std::fprintf(stderr, "[prof] module %5.1f%% %s\n", 100.0 * n / total, name.c_str());
                    }
                    std::vector<std::pair<uint32_t, DWORD64>> rows;
                    for (auto &[key, n] : leaf)
                        if (key.first == tid)
                            rows.emplace_back(n, key.second);
                    std::sort(rows.rbegin(), rows.rend());
                    for (size_t i = 0; i < rows.size() && i < 40; ++i)
                    {
                        std::fprintf(stderr, "[prof] leaf %5.1f%%", 100.0 * rows[i].first / total);
                        dodPrintModuleAddress(rows[i].second);
                        std::fprintf(stderr, "\n");
                    }
                    rows.clear();
                    for (auto &[key, n] : externalCaller)
                        if (key.first == tid)
                            rows.emplace_back(n, key.second);
                    std::sort(rows.rbegin(), rows.rend());
                    for (size_t i = 0; i < rows.size() && i < 25; ++i)
                        std::fprintf(stderr, "[prof] extcaller %5.1f%% dod.exe+%llx\n", 100.0 * rows[i].first / total,
                                     static_cast<unsigned long long>(rows[i].second));
                    rows.clear();
                    for (auto &[key, n] : inclusive)
                        if (key.first == tid)
                            rows.emplace_back(n, key.second);
                    std::sort(rows.rbegin(), rows.rend());
                    for (size_t i = 0; i < rows.size() && i < 60; ++i)
                        std::fprintf(stderr, "[prof] incl %5.1f%% dod.exe+%llx\n", 100.0 * rows[i].first / total,
                                     static_cast<unsigned long long>(rows[i].second));
                }
                for (auto &[tid, h] : threads)
                    CloseHandle(h);
                std::fflush(stderr); });
        }
#endif
        runtime.run();
        traceStop.store(true);
#if defined(_WIN32)
        if (hangWatchdog.joinable())
            hangWatchdog.join();
        if (profiler.joinable())
            profiler.join();
#endif
        if (tracer.joinable())
            tracer.join();
        std::cout.flush();
        std::cerr.flush();
        std::_Exit(0);
    }
    catch (const std::exception &e)
    {
        std::cerr << "[dod] fatal exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[dod] fatal exception: unknown" << std::endl;
    }
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(1);
}
