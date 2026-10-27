#include "ps2x/iop/iop_subsystem.h"

#include "iop_service.h"
#include "iop_module_manager.h"
#include "emulator/iop_emulator.h"
#include "module_factories.h"
#include "ps2x/iop/ps2_path.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <xmmintrin.h>
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
#endif

namespace ps2x::iop
{
    namespace
    {
        thread_local bool t_onIopThread = false;

        uint64_t nowNs()
        {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now().time_since_epoch())
                                             .count());
        }

        // [DoD] Threaded mode granularity (KillzoneRecomp patch 0015). The IOP thread runs the IOP in chunks of
        // kChunkEeCycles (4096 IOP cycles, ~110 us of IOP time) so EE-side callers waiting for the lock get in
        // quickly, and only wakes for work when at least kMinBacklogEeCycles (2048 IOP cycles) are unconsumed.
        constexpr uint64_t kChunkEeCycles = 4096u * 8u;
        constexpr uint64_t kMinBacklogEeCycles = 2048u * 8u;
    }

    // [DoD] DOD_STALL_WATCH=1 (diagnostics, off by default): heartbeats read by the runner's stall watcher
    // (runner/main.cpp): when the IOP thread last went round its loop and at which step (0 waiting for EE cycles, 1
    // waiting for the IOP lock, 2 running the IOP, 3 letting a waiting EE-side caller in), and since when an EE-side
    // caller has held the IOP lock (0 = not held). Steady-clock nanoseconds.
    std::atomic<uint64_t> g_dodIopBeatNs{0};
    std::atomic<uint32_t> g_dodIopStage{0};
    std::atomic<uint64_t> g_dodEeExecHoldSinceNs{0};
    bool dodStallWatchOn()
    {
        // On with DOD_STALL_WATCH=1 or the test launcher's audio trace (DOD_AUDIO_TRACE); DOD_STALL_WATCH=0 = off.
        static const bool on = []() {
            const char *v = std::getenv("DOD_STALL_WATCH");
            if (v && *v)
                return *v != '0';
            const char *trace = std::getenv("DOD_AUDIO_TRACE");
            return trace != nullptr && *trace != '\0';
        }();
        return on;
    }

    namespace
    {
        thread_local int t_execDepth = 0; // EE-side IOP lock nesting (ExecGuard)

        void dodIopBeat(uint32_t stage)
        {
            g_dodIopStage.store(stage, std::memory_order_relaxed);
            g_dodIopBeatNs.store(nowNs(), std::memory_order_relaxed);
        }
    }

    bool onIopThread() noexcept
    {
        return t_onIopThread;
    }

    class IopSubsystem::Impl
    {
    public:
        explicit Impl(IopHost &hostRef)
            : host(hostRef),
              emulator(hostRef)
        {
            const char *stats = std::getenv("DOD_IOP_THREAD_STATS");
            statsEnabled = stats && *stats && *stats != '0';
            coreServices.emplace_back(detail::createMcservService(host));
            coreServices.emplace_back(detail::createDbcmanService(host));
            coreServices.emplace_back(detail::createLibSdService(host));
            refreshServiceModuleKeys();
            rebuildRoutes();
        }

        ~Impl()
        {
            stopThread();
        }

        bool serviceActive(const detail::IopService &service) const
        {
            return moduleManager.isLoaded(service.moduleAliases());
        }

        void refreshServiceModuleKeys()
        {
            std::vector<std::string> keys;
            for (const auto &service : coreServices)
            {
                for (std::string_view alias : service->moduleAliases())
                    keys.emplace_back(alias);
            }
            moduleManager.setServiceModuleKeys(std::move(keys));
        }

        void rebuildRoutes()
        {
            routes.clear();
            lastError.clear();
            for (const auto &service : coreServices)
            {
                if (!serviceActive(*service))
                    continue;
                for (const uint32_t sid : service->sids())
                {
                    if (!routes.emplace(sid, service.get()).second)
                    {
                        std::ostringstream out;
                        out << "duplicate IOP SID 0x" << std::hex << sid << " in core services";
                        lastError = out.str();
                        routes.clear();
                        return;
                    }
                }
            }
        }

        void recordLoadOutcome(std::string_view path, bool hle)
        {
            constexpr size_t maxOutcomes = 32u;
            if (loadOutcomes.size() >= maxOutcomes || !loggedLoadPaths.emplace(path).second)
                return;
            std::string message = hle ? "[IOP:HLE] fallback module='" : "[IOP:load-failed] module='";
            message.append(path);
            message += hle ? "' physical IRX unavailable; using registered HLE provider"
                           : "' no HLE provider accepted the module; physical IRX was not loaded";
            loadOutcomes.push_back(message);
            host.log(hle ? LogLevel::Info : LogLevel::Warning, message);
        }

        // ---- [DoD] threaded mode (KillzoneRecomp patch 0015) ------------------------------------------------------
        // Every public IopSubsystem entry point takes execMutex (recursive: handleRpc -> HLE service -> host ->
        // readIopMemory re-enters). The IOP thread holds it while it runs the IOP. execWaiters lets the IOP thread
        // hand the lock over between chunks: std::mutex is not fair, and a tight unlock/lock loop would starve the EE.
        struct ExecGuard
        {
            Impl *impl = nullptr;
            explicit ExecGuard(const Impl &owner) noexcept
            {
                if (owner.threaded.load(std::memory_order_relaxed))
                {
                    impl = const_cast<Impl *>(&owner);
                    impl->lockExec();
                }
            }
            ~ExecGuard()
            {
                if (impl)
                {
                    if (dodStallWatchOn() && --t_execDepth == 0)
                        g_dodEeExecHoldSinceNs.store(0u, std::memory_order_relaxed);
                    impl->execMutex.unlock();
                }
            }
            ExecGuard(const ExecGuard &) = delete;
            ExecGuard &operator=(const ExecGuard &) = delete;
        };

        void lockExec()
        {
            execWaiters.fetch_add(1, std::memory_order_acq_rel);
            if (statsEnabled)
            {
                const uint64_t t0 = nowNs();
                execMutex.lock();
                eeWaitNs.fetch_add(nowNs() - t0, std::memory_order_relaxed);
                eeCalls.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                execMutex.lock();
            }
            execWaiters.fetch_sub(1, std::memory_order_acq_rel);
            if (dodStallWatchOn() && t_execDepth++ == 0)
                g_dodEeExecHoldSinceNs.store(nowNs(), std::memory_order_relaxed);
        }

        void startThread()
        {
            std::lock_guard<std::mutex> lock(threadStartMutex);
            if (threadStarted.load(std::memory_order_acquire) || !threaded.load(std::memory_order_acquire))
                return;
            stopRequested.store(false, std::memory_order_release);
#if defined(_WIN32)
            struct Start
            {
                static unsigned __stdcall entry(void *arg)
                {
                    static_cast<Impl *>(arg)->threadMain();
                    return 0;
                }
            };
            // 4 MB: IOP guest callbacks nest (callFunction re-entrancy) and the emulator keeps CPU states on the stack.
            threadHandle = reinterpret_cast<void *>(_beginthreadex(nullptr, 4u << 20, &Start::entry, this, 0, nullptr));
            threadStarted.store(threadHandle != nullptr, std::memory_order_release);
#else
            worker = std::thread([this]() { threadMain(); });
            threadStarted.store(true, std::memory_order_release);
#endif
        }

        void stopThread()
        {
            std::lock_guard<std::mutex> lock(threadStartMutex);
            if (!threadStarted.load(std::memory_order_acquire))
                return;
            stopRequested.store(true, std::memory_order_release);
#if defined(_WIN32)
            if (threadHandle)
            {
                WaitForSingleObject(static_cast<HANDLE>(threadHandle), INFINITE);
                CloseHandle(static_cast<HANDLE>(threadHandle));
                threadHandle = nullptr;
            }
#else
            if (worker.joinable())
                worker.join();
#endif
            threadStarted.store(false, std::memory_order_release);
        }

        // Sleep ~100 us (a high-resolution waitable timer on Windows; the default timer tick is 15.6 ms).
        static void idleWait(void *timer)
        {
#if defined(_WIN32)
            if (timer)
            {
                LARGE_INTEGER due;
                due.QuadPart = -1000; // 100 us, relative, in 100 ns units
                if (SetWaitableTimer(static_cast<HANDLE>(timer), &due, 0, nullptr, nullptr, FALSE))
                {
                    WaitForSingleObject(static_cast<HANDLE>(timer), 20);
                    return;
                }
            }
            Sleep(1);
#else
            (void)timer;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
#endif
        }

        void threadMain()
        {
            t_onIopThread = true;
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
            // Same floating-point environment as the EE thread (FTZ|DAZ), which used to run the IOP.
            _mm_setcsr(_mm_getcsr() | 0x8040u);
#endif
            void *timer = nullptr;
#if defined(_WIN32)
            if (const HMODULE kernel = GetModuleHandleW(L"kernel32.dll"))
            {
                using SetDesc = HRESULT(WINAPI *)(HANDLE, PCWSTR);
                if (const auto setDesc = reinterpret_cast<SetDesc>(GetProcAddress(kernel, "SetThreadDescription")))
                    setDesc(GetCurrentThread(), L"dod IOP");
            }
            // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION = 0x2 (Windows 10 1803+); falls back to Sleep(1) if unsupported.
            timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002u, TIMER_ALL_ACCESS);
#endif
            uint64_t consumedLocal = 0;
            uint64_t lastStatNs = nowNs();
            uint64_t statRunNs = 0;
            const bool watch = dodStallWatchOn(); // [DoD] DOD_STALL_WATCH heartbeats
            while (!stopRequested.load(std::memory_order_acquire))
            {
                const uint64_t target = published.load(std::memory_order_acquire);
                if (target - consumedLocal < kMinBacklogEeCycles)
                {
                    if (watch)
                        dodIopBeat(0u);
                    idleWait(timer);
                    continue;
                }
                while (consumedLocal < target && !stopRequested.load(std::memory_order_acquire))
                {
                    const uint64_t chunk = std::min<uint64_t>(target - consumedLocal, kChunkEeCycles);
                    if (watch)
                        dodIopBeat(1u);
                    {
                        std::lock_guard<std::recursive_mutex> lock(execMutex);
                        if (watch)
                            dodIopBeat(2u);
                        const uint64_t t0 = statsEnabled ? nowNs() : 0u;
                        emulator.runEeCycles(chunk);
                        if (statsEnabled)
                            statRunNs += nowNs() - t0;
                    }
                    consumedLocal += chunk;
                    // Let EE-side callers that queued for the lock in before taking it again.
                    while (execWaiters.load(std::memory_order_acquire) > 0 && !stopRequested.load(std::memory_order_acquire))
                    {
                        if (watch)
                            dodIopBeat(3u);
#if defined(_WIN32)
                        SwitchToThread();
#else
                        std::this_thread::yield();
#endif
                    }
                }
                if (statsEnabled)
                {
                    const uint64_t now = nowNs();
                    if (now - lastStatNs >= 5000000000ull)
                    {
                        const double wall = static_cast<double>(now - lastStatNs);
                        std::fprintf(stderr,
                                     "[iop-thread] busy %.1f%% of wall, lag %.2f ms, EE lock calls %llu, EE lock wait %.2f ms\n",
                                     100.0 * static_cast<double>(statRunNs) / wall,
                                     static_cast<double>(published.load(std::memory_order_relaxed) - consumedLocal) / 294912.0,
                                     static_cast<unsigned long long>(eeCalls.load(std::memory_order_relaxed)),
                                     static_cast<double>(eeWaitNs.load(std::memory_order_relaxed)) / 1.0e6);
                        lastStatNs = now;
                        statRunNs = 0;
                    }
                }
            }
#if defined(_WIN32)
            if (timer)
                CloseHandle(static_cast<HANDLE>(timer));
#endif
        }

        mutable std::recursive_mutex execMutex;
        std::atomic<int> execWaiters{0};
        std::atomic<bool> threaded{false};
        std::atomic<bool> threadStarted{false};
        std::atomic<bool> stopRequested{false};
        std::mutex threadStartMutex;
        std::atomic<uint64_t> published{0};
        std::atomic<uint64_t> eeCalls{0};
        std::atomic<uint64_t> eeWaitNs{0};
        bool statsEnabled = false;
#if defined(_WIN32)
        void *threadHandle = nullptr;
#else
        std::thread worker;
#endif

        IopHost &host;
        detail::ServiceList coreServices;
        std::unordered_map<uint32_t, detail::IopService *> routes;
        std::vector<std::string> loadOutcomes;
        std::unordered_set<std::string> loggedLoadPaths;
        std::string lastError;
        detail::IopModuleManager moduleManager;
        detail::IopEmulator emulator;
    };

    IopSubsystem::IopSubsystem(IopHost &host)
        : m_impl(std::make_unique<Impl>(host))
    {
    }

    IopSubsystem::~IopSubsystem() = default;
    IopSubsystem::IopSubsystem(IopSubsystem &&) noexcept = default;
    IopSubsystem &IopSubsystem::operator=(IopSubsystem &&) noexcept = default;

    void IopSubsystem::reset()
    {
        const Impl::ExecGuard guard(*m_impl);
        m_impl->moduleManager.reset();
        m_impl->loadOutcomes.clear();
        m_impl->loggedLoadPaths.clear();
        for (auto &service : m_impl->coreServices)
        {
            if (service)
            {
                service->reset();
            }
        }
        m_impl->emulator.reset();
        m_impl->refreshServiceModuleKeys();
        m_impl->rebuildRoutes();
    }

    ModuleLoadResult IopSubsystem::loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
    {
        const Impl::ExecGuard guard(*m_impl);
        const ParsedPs2Path parsed = parsePs2Path(path);
        if (!parsed)
            return {true, -1, -1};

        if (parsed.device != Ps2PathDevice::Rom0)
        {
            ModuleLoadResult physical = m_impl->emulator.loadModule(path, arguments, argumentSize);
            if (physical.moduleId > 0)
            {
                m_impl->moduleManager.observePhysicalLoad(physical.moduleId, path);
                m_impl->rebuildRoutes();
                return physical;
            }
        }

        ModuleLoadResult hle = m_impl->moduleManager.loadHle(path);
        if (hle.moduleId > 0)
        {
            m_impl->rebuildRoutes();
            if (parsed.device != Ps2PathDevice::Rom0)
                m_impl->recordLoadOutcome(path, true);
        }
        else
        {
            m_impl->recordLoadOutcome(path, false);
        }
        return hle;
    }

    ModuleLoadResult IopSubsystem::loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
    {
        const Impl::ExecGuard guard(*m_impl);
        return m_impl->emulator.loadModuleBuffer(guestAddress, arguments, argumentSize);
    }

    bool IopSubsystem::stopModule(int32_t moduleId, int32_t *result)
    {
        const Impl::ExecGuard guard(*m_impl);
        if (m_impl->moduleManager.stopHle(moduleId, result))
        {
            m_impl->rebuildRoutes();
            return true;
        }
        if (!m_impl->emulator.stopModule(moduleId, result))
            return false;
        m_impl->moduleManager.observePhysicalStop(moduleId);
        m_impl->rebuildRoutes();
        return true;
    }

    void IopSubsystem::runEeCycles(uint64_t eeCycles) noexcept
    {
        const Impl::ExecGuard guard(*m_impl);
        m_impl->emulator.runEeCycles(eeCycles);
    }

    RpcAbi IopSubsystem::selectRpcAbi(const RpcAbiRequest &request) const
    {
        const Impl::ExecGuard guard(*m_impl);
        for (const auto &service : m_impl->coreServices)
        {
            if (service && m_impl->serviceActive(*service))
            {
                const RpcAbi selected = service->selectRpcAbi(request);
                if (selected != RpcAbi::RuntimeDefault)
                {
                    return selected;
                }
            }
        }
        return RpcAbi::RuntimeDefault;
    }

    bool IopSubsystem::canBindRpc(uint32_t sid) const noexcept
    {
        const Impl::ExecGuard guard(*m_impl);
        if (m_impl->routes.find(sid) != m_impl->routes.end())
        {
            return true;
        }
        return m_impl->emulator.hasRpcServer(sid);
    }

    RpcResult IopSubsystem::handleRpc(const RpcRequest &request)
    {
        const Impl::ExecGuard guard(*m_impl);
        const auto route = m_impl->routes.find(request.sid);
        detail::IopService *hle = route != m_impl->routes.end() ? route->second : nullptr;

        RpcResult emulated = m_impl->emulator.handleRpc(request);
        if (emulated.handled || !hle)
        {
            return emulated;
        }
        return hle->handleRpc(request);
    }

    void IopSubsystem::changeMedia(const std::function<void()> &hostUpdate)
    {
        const Impl::ExecGuard guard(*m_impl);
        if (hostUpdate)
            hostUpdate();
        m_impl->emulator.changeMedia();
    }

    void IopSubsystem::onSifTransfer(const SifTransfer &transfer)
    {
        const Impl::ExecGuard guard(*m_impl);
        for (auto &service : m_impl->coreServices)
        {
            if (service && m_impl->serviceActive(*service))
            {
                service->onSifTransfer(transfer);
            }
        }
        m_impl->emulator.onSifTransfer(transfer);
    }

    uint32_t IopSubsystem::allocateMemory(uint32_t size, uint32_t alignment)
    {
        const Impl::ExecGuard guard(*m_impl);
        return m_impl->emulator.allocateMemory(size, alignment);
    }

    bool IopSubsystem::freeMemory(uint32_t address)
    {
        const Impl::ExecGuard guard(*m_impl);
        return m_impl->emulator.freeMemory(address);
    }

    bool IopSubsystem::readMemory(uint32_t address, void *destination, size_t size) const
    {
        const Impl::ExecGuard guard(*m_impl);
        return m_impl->emulator.readMemory(address, destination, size);
    }

    bool IopSubsystem::writeMemory(uint32_t address, const void *source, size_t size)
    {
        const Impl::ExecGuard guard(*m_impl);
        return m_impl->emulator.writeMemory(address, source, size);
    }

    bool IopSubsystem::zeroMemory(uint32_t address, size_t size)
    {
        const Impl::ExecGuard guard(*m_impl);
        return m_impl->emulator.zeroMemory(address, size);
    }

    bool IopSubsystem::isMemoryRange(uint32_t address, size_t size) const
    {
        return m_impl->emulator.isMemoryRange(address, size);
    }

    DebugSnapshot IopSubsystem::debugSnapshot() const
    {
        const Impl::ExecGuard guard(*m_impl);
        DebugSnapshot snapshot;
        snapshot.emulatorCycles = m_impl->emulator.cycles();
        snapshot.emulatorInstructions = m_impl->emulator.instructions();
        snapshot.emulatorLoadedModules = m_impl->emulator.loadedModuleCount();
        snapshot.emulatorThreads = m_impl->emulator.threadCount();
        snapshot.threads = m_impl->emulator.describeThreads();
        snapshot.emulatorRpcServers = m_impl->emulator.rpcServerCount();
        snapshot.diagnostics = m_impl->loadOutcomes;
        if (!m_impl->lastError.empty())
        {
            snapshot.diagnostics.push_back(m_impl->lastError);
        }

        for (const auto &service : m_impl->coreServices)
        {
            DebugService row;
            row.name = service->name();
            row.sids.assign(service->sids().begin(), service->sids().end());
            row.active = m_impl->serviceActive(*service);
            service->appendDebugMetrics(row.metrics);
            snapshot.services.push_back(std::move(row));
        }
        return snapshot;
    }

    void IopSubsystem::enableThread(bool enable)
    {
        if (enable)
        {
            m_impl->threaded.store(true, std::memory_order_release);
            return;
        }
        m_impl->stopThread();
        m_impl->threaded.store(false, std::memory_order_release);
    }

    bool IopSubsystem::threadEnabled() const noexcept
    {
        return m_impl->threaded.load(std::memory_order_acquire);
    }

    void IopSubsystem::publishEeCycles(uint64_t totalEeCycles) noexcept
    {
        if (!m_impl->threaded.load(std::memory_order_relaxed))
            return;
        // [DoD] Never backwards: the runtime's EE stall watchdog may publish ahead of the EE (real time while the EE
        // thread is blocked) from its own thread, and the IOP thread counts on target >= what it consumed.
        uint64_t current = m_impl->published.load(std::memory_order_relaxed);
        while (totalEeCycles > current &&
               !m_impl->published.compare_exchange_weak(current, totalEeCycles, std::memory_order_release, std::memory_order_relaxed))
        {
        }
        if (!m_impl->threadStarted.load(std::memory_order_relaxed))
            m_impl->startThread();
    }
}
