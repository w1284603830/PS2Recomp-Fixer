#pragma once

#include "iop_cpu.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ps2x::iop::detail
{
    class IopMemory;

    enum class IopThreadState : uint8_t
    {
        Dormant,
        Ready,
        Running,
        Sleep,
        Delay,
        Semaphore,
        EventFlag,
        Suspended,
        Dead,
    };

    struct IopThread
    {
        int id = 0;
        IopThreadState state = IopThreadState::Dormant;
        IopCpuState cpu;
        uint32_t entry = 0;
        uint32_t stackBase = 0;
        uint32_t stackSize = 0;
        uint32_t priority = 0x40;
        uint32_t initialPriority = 0x40;
        uint32_t option = 0;
        uint32_t attr = 0;
        uint64_t wakeCycle = 0;
        int waitId = 0;
        uint32_t waitBits = 0;
        uint32_t waitMode = 0;
        uint32_t waitResultAddress = 0;
        int wakeupCount = 0;
    };

    class IopKernel
    {
    public:
        explicit IopKernel(IopMemory &memory) noexcept;

        void reset();

        [[nodiscard]] bool dispatchThreadImport(uint16_t ordinal, IopCpuState &cpu, uint64_t currentCycle);
        [[nodiscard]] bool dispatchSemaphoreImport(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] bool dispatchEventImport(uint16_t ordinal, IopCpuState &cpu);

        [[nodiscard]] int createInternalEventFlag(uint32_t attr, uint32_t option, uint32_t bits);
        [[nodiscard]] bool setInternalEventFlag(int id, uint32_t bits);

        void sleepCurrent(IopCpuState &cpu);
        void delayCurrentUntil(uint64_t wakeCycle, IopCpuState &cpu);
        // [DoD] for sceSifRpcLoop: the thread running now (0 outside a thread timeslice) and waking a sleeping thread.
        [[nodiscard]] int currentThreadId() const noexcept { return m_currentThread ? m_currentThread->id : 0; }
        bool wakeSleepingThread(int id);

        // [DoD] Upstream PR #266: code running outside any IOP thread (RPC server functions, module start routines)
        // that must wait for a semaphore. While set, SignalSema(id) keeps the count for that waiter instead of
        // waking a kernel waiter, and ends the signalling thread's slice. 0 = none.
        [[nodiscard]] bool inThread() const noexcept { return m_currentThread != nullptr; }
        [[nodiscard]] int semaphoreCount(int id) const
        {
            const auto semaphore = m_semaphores.find(id);
            return semaphore == m_semaphores.end() ? -1 : semaphore->second.current;
        }
        void setOutsideSemaphoreWait(int id) noexcept { m_outsideSemaphoreWait = id; }
        // [DoD] stickyId: the thread whose timeslice just ended while still ready. The IOP kernel has no timeslices:
        // it keeps the CPU unless a thread of higher priority is ready, and resumes before other ready threads of its
        // priority once that higher one blocks (0 = upstream selection: lowest number, then lowest id).
        [[nodiscard]] IopThread *beginNextReady(uint64_t currentCycle, int stickyId = 0);
        // [DoD] Run this thread next (a thread whose timeslice ended inside CpuSuspendIntr) and look a thread up.
        [[nodiscard]] IopThread *beginThread(int id);
        [[nodiscard]] IopThread *findThread(int id);
        [[nodiscard]] uint64_t nextWakeCycle(uint64_t fallback) const;
        void endTimeslice(IopThread &thread, uint32_t returnSentinel);
        void cleanupDeadThreads();
        void terminateThreadsInRange(uint32_t base, uint32_t size);

        [[nodiscard]] size_t threadCount() const noexcept { return m_threads.size(); }
        // [DoD] one line per thread (id, state, pc, entry, wait object) for the host's --trace output.
        [[nodiscard]] std::vector<std::string> describeThreads() const;

    private:
        struct Semaphore
        {
            int id = 0;
            uint32_t attr = 0;
            uint32_t option = 0;
            int current = 0;
            int maximum = 1;
        };

        struct EventFlag
        {
            int id = 0;
            uint32_t bits = 0;
            uint32_t attr = 0;
            uint32_t option = 0;
        };

        [[nodiscard]] bool referThreadStatus(int id, uint32_t outputAddress);
        void wakeOneSemaphore(int id);
        [[nodiscard]] static bool eventSatisfied(const EventFlag &event, uint32_t bits, uint32_t mode);
        void wakeEventWaiters(EventFlag &event);

        IopMemory &m_memory;
        std::map<int, IopThread> m_threads;
        std::map<int, Semaphore> m_semaphores;
        std::map<int, EventFlag> m_eventFlags;
        uint32_t m_nextThreadId = 1;
        uint32_t m_nextSemaphoreId = 1;
        uint32_t m_nextEventFlagId = 1;
        IopThread *m_currentThread = nullptr;
        std::map<uint32_t, int> m_resumeByPriority; // [DoD] a thread preempted by a higher priority, per priority
        int m_outsideSemaphoreWait = 0; // [DoD] see setOutsideSemaphoreWait
    };
}
