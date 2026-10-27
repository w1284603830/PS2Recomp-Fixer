#pragma once

#include "ps2x/iop/iop_types.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ps2x::iop
{
    class IopHost;
}

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopKernel;
    class IopMemory;

    class IopGuestExecutor
    {
    public:
        virtual ~IopGuestExecutor() = default;

        [[nodiscard]] virtual uint32_t executeGuestFunction(uint32_t address,
                                                            uint32_t a0,
                                                            uint32_t a1,
                                                            uint32_t a2,
                                                            uint32_t a3,
                                                            uint32_t gp) = 0;
        [[nodiscard]] virtual uint32_t executeGuestFunctionWithBudget(uint32_t address,
                                                                      uint32_t a0,
                                                                      uint32_t a1,
                                                                      uint32_t a2,
                                                                      uint32_t a3,
                                                                      uint32_t gp,
                                                                      uint32_t instructionBudget)
        {
            return executeGuestFunction(address, a0, a1, a2, a3, gp);
        }
    };

    class IopRpcBridge
    {
    public:
        IopRpcBridge(IopHost &host, IopMemory &memory, IopKernel &kernel) noexcept;

        void reset();
        [[nodiscard]] bool dispatchSifManImport(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] bool dispatchSifCmdImport(uint16_t ordinal, IopCpuState &cpu, uint64_t currentCycle);
        [[nodiscard]] RpcResult handleRpc(const RpcRequest &request, IopGuestExecutor &executor);
        void onSifTransfer(const SifTransfer &transfer);
        void removeServersInRange(uint32_t base, uint32_t size);

        // [DoD] Requests served by the server's own thread inside sceSifRpcLoop, as on hardware, so a server function
        // may block (LIBSD sceSdVoiceTransStatus waits on an event flag for the SPU2 DMA interrupt).
        // queueRequest returns 0 when no thread serves that SID's queue (the caller then uses handleRpc).
        [[nodiscard]] uint64_t queueRequest(const RpcRequest &request);
        [[nodiscard]] bool isRequestComplete(uint64_t id) const;
        void forgetRequest(uint64_t id);
        // sifcmd #22 sceSifRpcLoop for the current thread: redirects cpu.pc (into the next server call, or back to the
        // import stub after putting the thread to sleep). Returns false when there is no current thread.
        [[nodiscard]] bool serviceRpcLoop(IopCpuState &cpu, int threadId);

        [[nodiscard]] bool hasServer(uint32_t sid) const noexcept;
        [[nodiscard]] size_t serverCount() const noexcept { return m_servers.size(); }

    private:
        struct QueuedCall
        {
            RpcRequest request;
            std::vector<uint8_t> payload; // EE send data, captured when the call was made
            bool done = false;
        };

        struct LoopThread
        {
            uint32_t queue = 0;
            uint64_t inFlight = 0;
            uint32_t savedRa = 0;
            uint32_t savedGp = 0;
            uint32_t savedSp = 0;
        };

        void completeQueuedCall(QueuedCall &call, uint32_t returnPointer);

        struct RpcServer
        {
            uint32_t sid = 0;
            uint32_t serverData = 0;
            uint32_t function = 0;
            uint32_t gp = 0;
            uint32_t buffer = 0;
            uint32_t callback = 0;
            uint32_t callbackBuffer = 0;
            uint32_t queue = 0;
        };

        IopHost &m_host;
        IopMemory &m_memory;
        IopKernel &m_kernel;
        std::unordered_map<uint32_t, RpcServer> m_servers;
        uint32_t m_nextDmaId = 1u;
        bool m_sifInitialized = false;
        std::unordered_set<uint32_t> m_boundEeSids; // [DoD] for logging the first IOP->EE bind per SID
        bool m_warnedCallRpc = false;
        std::unordered_map<uint32_t, int> m_queueThreads;                 // queue data address -> serving thread
        std::unordered_map<uint32_t, std::deque<uint64_t>> m_queuedByQueue; // queue data address -> waiting calls
        std::unordered_map<uint64_t, QueuedCall> m_queuedCalls;
        std::unordered_map<int, LoopThread> m_loopThreads;
        uint64_t m_nextQueuedCall = 1u;
    };
}
