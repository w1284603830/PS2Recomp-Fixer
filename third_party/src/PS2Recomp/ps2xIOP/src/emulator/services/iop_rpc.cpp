#include "iop_rpc.h"

#include "../core/iop_cpu.h"
#include "../core/iop_kernel.h"
#include "../core/iop_memory.h"
#include "ps2x/iop/iop_host.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace ps2x::iop::detail
{
    IopRpcBridge::IopRpcBridge(IopHost &host, IopMemory &memory, IopKernel &kernel) noexcept
        : m_host(host), m_memory(memory), m_kernel(kernel)
    {
    }

    void IopRpcBridge::reset()
    {
        m_servers.clear();
        m_nextDmaId = 1u;
        // [DoD] On hardware the IOP boot ROM's SIFMAN calls sceSifInit before any game IRX runs, so game modules
        // (DBCMAN: "yet sif hasn't been init") must see sceSifCheckInit() == 1 from the start.
        m_sifInitialized = true;
        m_boundEeSids.clear();
        m_warnedCallRpc = false;
        m_queueThreads.clear();
        m_queuedByQueue.clear();
        m_queuedCalls.clear();
        m_loopThreads.clear();
        m_nextQueuedCall = 1u;
    }

    bool IopRpcBridge::dispatchSifManImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };
        switch (ordinal)
        {
        case 4: // sceSifDma2Init
        case 5: // sceSifInit
            m_sifInitialized = true;
            setV0(0u);
            return true;
        case 7: // sceSifSetDma
        case 32: // sceSifSetDmaIntr: same transfer; executor schedules completion.
        {
            constexpr uint32_t kDescriptorSize = 16u;
            constexpr uint32_t kMaxDescriptors = 32u;
            const uint32_t descriptorAddress = cpu.gpr[4];
            const uint32_t descriptorCount = cpu.gpr[5];
            const auto traceDmaFailure = [&](const char *reason, uint32_t source, uint32_t destination, uint32_t size, uint32_t attr)
            {
                static const bool enabled = std::getenv("DOD_PAD_TRACE") != nullptr;
                static unsigned count = 0;
                if (enabled && count++ < 48u)
                    std::fprintf(stderr, "[pad:sifdma] fail=%s descriptors=%08X count=%u source=%08X dest=%08X size=%u attr=%08X\n",
                                 reason, descriptorAddress, descriptorCount, source, destination, size, attr);
            };
            if (descriptorAddress == 0u || descriptorCount == 0u || descriptorCount > kMaxDescriptors)
            {
                traceDmaFailure("descriptor-count", 0, 0, 0, 0);
                setV0(0u);
                return true;
            }

            struct PendingTransfer
            {
                uint32_t source = 0u;
                uint32_t destination = 0u;
                uint32_t size = 0u;
            };

            std::array<uint32_t, kMaxDescriptors * 4u> descriptorWords{};
            const size_t descriptorBytes = static_cast<size_t>(descriptorCount) * kDescriptorSize;
            if (!m_memory.readRam(descriptorAddress, descriptorWords.data(), descriptorBytes))
            {
                traceDmaFailure("descriptor-read", 0, 0, 0, 0);
                setV0(0u);
                return true;
            }

            std::array<PendingTransfer, kMaxDescriptors> pending{};
            uint32_t pendingCount = 0u;
            uint32_t largestTransfer = 0u;
            for (uint32_t i = 0u; i < descriptorCount; ++i)
            {
                const uint32_t source = descriptorWords[i * 4u + 0u];
                const uint32_t destination = descriptorWords[i * 4u + 1u];
                const int32_t signedSize = static_cast<int32_t>(descriptorWords[i * 4u + 2u]);
                if (signedSize <= 0)
                    continue;

                const uint32_t size = static_cast<uint32_t>(signedSize);
                if (!m_memory.ownsRamRange(source, size))
                {
                    traceDmaFailure("source-ownership", source, destination, size, descriptorWords[i * 4u + 3u]);
                    setV0(0u);
                    return true;
                }
                pending[pendingCount++] = {source, destination, size};
                largestTransfer = std::max(largestTransfer, size);
            }

            // IOP-side sceSifSetDma sends IOP RAM to the EE. Validate all EE
            // destinations before committing any write so a bad chain cannot
            // partially update guest memory, but maybe we could skip this check if we trust the EE-side SIF driver to validate the chain ?!
            // TODO check later
            std::vector<uint8_t> scratch(largestTransfer);
            for (uint32_t i = 0u; i < pendingCount; ++i)
            {
                const PendingTransfer &transfer = pending[i];
                if (!m_host.readGuest(transfer.destination, scratch.data(), transfer.size))
                {
                    traceDmaFailure("ee-destination", transfer.source, transfer.destination, transfer.size, 0);
                    setV0(0u);
                    return true;
                }
            }

            for (uint32_t i = 0u; i < pendingCount; ++i)
            {
                const PendingTransfer &transfer = pending[i];
                if (!m_memory.readRam(transfer.source, scratch.data(), transfer.size) || !m_host.writeGuest(transfer.destination, scratch.data(), transfer.size))
                {
                    traceDmaFailure("transfer", transfer.source, transfer.destination, transfer.size, 0);
                    setV0(0u);
                    return true;
                }
            }

            const uint32_t dmaId = m_nextDmaId++;
            if (m_nextDmaId == 0u || m_nextDmaId > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
            {
                m_nextDmaId = 1u;
            }
            setV0(dmaId);
            return true;
        }
        case 8: // sceSifDmaStat
            setV0(0xFFFFFFFFu);
            return true;
        case 29: // sceSifCheckInit
            setV0(m_sifInitialized ? 1u : 0u);
            return true;
        default:
            setV0(0u);
            return true;
        }
    }

    bool IopRpcBridge::dispatchSifCmdImport(uint16_t ordinal, IopCpuState &cpu, uint64_t currentCycle)
    {
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };
        switch (ordinal)
        {
        case 4: // InitCmd
        case 5:
        case 6:
        case 7:
        case 8:
        case 9:
        case 10:
        case 11:
        case 14: // InitRpc
            setV0(0);
            return true;
        case 15: // sceSifBindRpc: an IOP client binding to a server on the EE
        {
            // [DoD] SNDDRV binds SID 0x77777779 (the game's EE server) and then DMAs status into the server buffer
            // it gets back. On hardware the call sleeps until the EE answers, with cd->server == NULL when the EE
            // has not registered the SID yet; drivers retry. Sleeping matters: SNDDRV retries in a loop that never
            // yields, so an instant return would starve its own server threads.
            constexpr uint32_t kClientSize = 0x28u;
            constexpr uint64_t kReplyCycles = 0x4000u;
            const uint32_t client = cpu.gpr[4];
            const uint32_t sid = cpu.gpr[5];
            uint32_t server = 0u, buff = 0u, cbuff = 0u;
            if (client != 0u && m_memory.ownsRamRange(client, kClientSize))
            {
                if (m_host.lookupEeRpcServer(sid, server, buff, cbuff))
                {
                    m_memory.write32(client + 0x14u, buff);
                    m_memory.write32(client + 0x18u, cbuff);
                    if (m_boundEeSids.insert(sid).second)
                    {
                        char line[96];
                        std::snprintf(line, sizeof(line), "[IOP] sceSifBindRpc sid=0x%08X -> EE server 0x%08X buff=0x%08X", sid, server, buff);
                        m_host.log(LogLevel::Info, line);
                    }
                }
                m_memory.write32(client + 0x24u, server);
            }
            m_kernel.delayCurrentUntil(currentCycle + kReplyCycles, cpu);
            setV0(0);
            return true;
        }
        case 16: // sceSifCallRpc (IOP client -> EE server): not emulated yet
            if (!m_warnedCallRpc)
            {
                m_warnedCallRpc = true;
                m_host.log(LogLevel::Warning, "[IOP] sceSifCallRpc to an EE server is not emulated");
            }
            setV0(0);
            return true;
        case 12: // sceSifSendCmd
        case 13: // isceSifSendCmd
        {
            constexpr uint32_t kHeaderSize = 16u;
            constexpr uint32_t kMaxPacketSize = 112u;
            const uint32_t commandId = cpu.gpr[4];
            const uint32_t packetAddress = cpu.gpr[5];
            const uint32_t packetSize = cpu.gpr[6];
            const uint32_t extraSource = cpu.gpr[7];
            const uint32_t stackPointer = cpu.gpr[29];
            const uint32_t extraDestination = m_memory.read32(stackPointer + 16u);
            const int32_t signedExtraSize = static_cast<int32_t>(m_memory.read32(stackPointer + 20u));

            if (packetAddress == 0u || packetSize < kHeaderSize || packetSize > kMaxPacketSize ||
                !m_memory.ownsRamRange(packetAddress, packetSize))
            {
                setV0(0u);
                return true;
            }

            std::array<uint8_t, kMaxPacketSize> packet{};
            if (!m_memory.readRam(packetAddress, packet.data(), packetSize))
            {
                setV0(0u);
                return true;
            }

            uint32_t extraSize = 0u;
            if (signedExtraSize > 0)
            {
                extraSize = static_cast<uint32_t>(signedExtraSize);
                if (extraSource == 0u || extraDestination == 0u ||
                    !m_memory.ownsRamRange(extraSource, extraSize) ||
                    !m_host.writeGuest(extraDestination, m_memory.ram().data() + IopMemory::physicalAddress(extraSource), extraSize))
                {
                    setV0(0u);
                    return true;
                }
            }

            const uint32_t sizeWord = packetSize | (extraSize << 8u);
            std::memcpy(packet.data() + 0u, &sizeWord, sizeof(sizeWord));
            std::memcpy(packet.data() + 4u, &extraDestination, sizeof(extraDestination));
            std::memcpy(packet.data() + 8u, &commandId, sizeof(commandId));

            if (!m_host.sendSifCommand(commandId, packet.data(), packetSize))
            {
                // A command without an EE handler is still a completed DMA on  real hardware. Only malformed packets fail above.
            }

            const uint32_t dmaId = m_nextDmaId++;
            if (m_nextDmaId == 0u || m_nextDmaId > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
                m_nextDmaId = 1u;
            setV0(dmaId);
            return true;
        }
        case 17: // sceSifRegisterRpc
        {
            RpcServer server;
            server.serverData = cpu.gpr[4];
            server.sid = cpu.gpr[5];
            server.function = cpu.gpr[6];
            server.gp = cpu.gpr[28];
            server.buffer = cpu.gpr[7];
            const uint32_t stackPointer = cpu.gpr[29];
            server.callback = m_memory.read32(stackPointer + 16u);
            server.callbackBuffer = m_memory.read32(stackPointer + 20u);
            server.queue = m_memory.read32(stackPointer + 24u);
            m_servers[server.sid] = server;
            if (server.serverData != 0u)
            {
                m_memory.write32(server.serverData + 0x20u, server.sid);
                m_memory.write32(server.serverData + 0x28u, server.function);
                m_memory.write32(server.serverData + 0x2Cu, server.buffer);
            }
            setV0(server.serverData);
            return true;
        }
        case 18:
            setV0(0);
            return true;
        case 19: // SetRpcQueue(qd, thread_id)
            if (cpu.gpr[4] != 0u && cpu.gpr[5] != 0u)
                m_queueThreads[cpu.gpr[4]] = static_cast<int>(cpu.gpr[5]);
            setV0(cpu.gpr[4]);
            return true;
        case 20:
        case 21:
            setV0(0);
            return true;
        case 22: // RpcLoop
            m_kernel.sleepCurrent(cpu);
            setV0(0);
            return true;
        case 23: // sceSifGetOtherData(rd, eeSource, iopDest, size, mode): pull EE memory into IOP RAM
        {
            // [DoD] From KillzoneRecomp patch 0006: sound drivers pull their banks from EE RAM this way; as a no-op
            // the IOP buffer kept whatever it held before.
            const uint32_t source = cpu.gpr[5];
            const uint32_t destination = cpu.gpr[6];
            const int32_t size = static_cast<int32_t>(cpu.gpr[7]);
            if (size > 0 && static_cast<uint32_t>(size) <= IopMemory::RamSize)
            {
                std::vector<uint8_t> data(static_cast<size_t>(size));
                if (!m_host.readGuest(source, data.data(), data.size()) ||
                    !m_memory.writeRam(destination, data.data(), data.size()))
                {
                    setV0(static_cast<uint32_t>(-1));
                    return true;
                }
            }
            setV0(0);
            return true;
        }
        case 24: // RemoveRpc
        {
            const uint32_t serverData = cpu.gpr[4];
            for (auto server = m_servers.begin(); server != m_servers.end(); ++server)
            {
                if (server->second.serverData == serverData)
                {
                    m_servers.erase(server);
                    break;
                }
            }
            setV0(0);
            return true;
        }
        case 25:
        case 26:
        case 27:
        case 28:
        case 29:
            setV0(0);
            return true;
        default:
            return false;
        }
    }

    RpcResult IopRpcBridge::handleRpc(const RpcRequest &request, IopGuestExecutor &executor)
    {
        RpcResult result{};
        const auto serverIt = m_servers.find(request.sid);
        if (serverIt == m_servers.end() || serverIt->second.function == 0u)
            return result;

        RpcServer &server = serverIt->second;
        if (request.send.size != 0u && server.buffer != 0u)
        {
            const uint32_t copySize = std::min<uint32_t>(request.send.size, IopMemory::RamSize - std::min(server.buffer, IopMemory::RamSize));
            if (copySize != 0u)
            {
                std::vector<uint8_t> payload(copySize);
                if (m_host.readGuest(request.send.address, payload.data(), payload.size()))
                    (void)m_memory.writeRam(server.buffer, payload.data(), payload.size());
            }
        }

        uint32_t returnPointer = executor.executeGuestFunction(server.function,
                                                               request.function,
                                                               server.buffer,
                                                               request.send.size,
                                                               0u,
                                                               server.gp);
        if (returnPointer == 0u)
            returnPointer = server.buffer;
        if (request.receive.address != 0u && request.receive.size != 0u && returnPointer != 0u)
        {
            const uint32_t physical = IopMemory::physicalAddress(returnPointer);
            if (physical < IopMemory::RamSize)
            {
                const uint32_t copySize = std::min<uint32_t>(request.receive.size, IopMemory::RamSize - physical);
                (void)m_host.writeGuest(request.receive.address, m_memory.ram().data() + physical, copySize);
                if (copySize < request.receive.size)
                    (void)m_host.zeroGuest(request.receive.address + copySize, request.receive.size - copySize);
            }
        }

        result.handled = true;
        result.resultAddress = request.receive.address;
        result.serverDispatchPolicy = ServerDispatchPolicy::Suppress;
        result.signalNowaitCompletion = true;
        result.signalCompletion = true;
        return result;
    }

    uint64_t IopRpcBridge::queueRequest(const RpcRequest &request)
    {
        const auto serverIt = m_servers.find(request.sid);
        if (serverIt == m_servers.end() || serverIt->second.function == 0u || serverIt->second.queue == 0u)
            return 0u;
        const uint32_t queue = serverIt->second.queue;
        const auto threadIt = m_queueThreads.find(queue);
        if (threadIt == m_queueThreads.end())
            return 0u;

        QueuedCall call;
        call.request = request;
        if (request.send.size != 0u && serverIt->second.buffer != 0u)
        {
            const uint32_t limit = IopMemory::RamSize - std::min(IopMemory::physicalAddress(serverIt->second.buffer), IopMemory::RamSize);
            call.payload.resize(std::min<uint32_t>(request.send.size, limit));
            if (!call.payload.empty() && !m_host.readGuest(request.send.address, call.payload.data(), call.payload.size()))
                call.payload.clear();
        }
        const uint64_t id = m_nextQueuedCall++;
        m_queuedCalls.emplace(id, std::move(call));
        m_queuedByQueue[queue].push_back(id);
        (void)m_kernel.wakeSleepingThread(threadIt->second);
        return id;
    }

    bool IopRpcBridge::isRequestComplete(uint64_t id) const
    {
        const auto call = m_queuedCalls.find(id);
        return call == m_queuedCalls.end() || call->second.done;
    }

    void IopRpcBridge::forgetRequest(uint64_t id)
    {
        const auto call = m_queuedCalls.find(id);
        if (call != m_queuedCalls.end() && call->second.done)
            m_queuedCalls.erase(call);
        // An unfinished call stays queued; its server thread completes it later.
    }

    void IopRpcBridge::completeQueuedCall(QueuedCall &call, uint32_t returnPointer)
    {
        const RpcRequest &request = call.request;
        if (request.receive.address != 0u && request.receive.size != 0u && returnPointer != 0u)
        {
            const uint32_t physical = IopMemory::physicalAddress(returnPointer);
            if (physical < IopMemory::RamSize)
            {
                const uint32_t copySize = std::min<uint32_t>(request.receive.size, IopMemory::RamSize - physical);
                (void)m_host.writeGuest(request.receive.address, m_memory.ram().data() + physical, copySize);
                if (copySize < request.receive.size)
                    (void)m_host.zeroGuest(request.receive.address + copySize, request.receive.size - copySize);
            }
        }
        call.done = true;
    }

    bool IopRpcBridge::serviceRpcLoop(IopCpuState &cpu, int threadId)
    {
        if (threadId == 0)
            return false;
        const uint32_t stub = cpu.pc;
        LoopThread &loop = m_loopThreads[threadId];
        if (loop.inFlight != 0u)
        {
            // The server function returned into the sceSifRpcLoop stub: send its reply (v0 points at it).
            const auto call = m_queuedCalls.find(loop.inFlight);
            if (call != m_queuedCalls.end())
            {
                uint32_t returnPointer = cpu.gpr[2];
                if (returnPointer == 0u)
                {
                    const auto server = m_servers.find(call->second.request.sid);
                    returnPointer = server != m_servers.end() ? server->second.buffer : 0u;
                }
                completeQueuedCall(call->second, returnPointer);
            }
            loop.inFlight = 0u;
            cpu.gpr[4] = loop.queue;
            cpu.gpr[28] = loop.savedGp;
            cpu.gpr[29] = loop.savedSp;
            cpu.gpr[31] = loop.savedRa;
        }
        else
        {
            loop.queue = cpu.gpr[4];
            loop.savedGp = cpu.gpr[28];
            loop.savedSp = cpu.gpr[29];
            loop.savedRa = cpu.gpr[31];
            if (loop.queue != 0u)
                m_queueThreads[loop.queue] = threadId;
        }

        auto &waiting = m_queuedByQueue[loop.queue];
        while (!waiting.empty())
        {
            const uint64_t id = waiting.front();
            waiting.pop_front();
            const auto call = m_queuedCalls.find(id);
            if (call == m_queuedCalls.end())
                continue;
            const auto server = m_servers.find(call->second.request.sid);
            if (server == m_servers.end() || server->second.function == 0u)
            {
                call->second.done = true;
                continue;
            }
            if (!call->second.payload.empty())
                (void)m_memory.writeRam(server->second.buffer, call->second.payload.data(), call->second.payload.size());
            loop.inFlight = id;
            cpu.gpr[4] = call->second.request.function;
            cpu.gpr[5] = server->second.buffer;
            cpu.gpr[6] = call->second.request.send.size;
            cpu.gpr[7] = 0u;
            cpu.gpr[28] = server->second.gp;
            cpu.gpr[31] = stub; // returning re-enters sceSifRpcLoop
            cpu.pc = server->second.function;
            cpu.branchPending = false;
            return true;
        }

        // Nothing queued: sleep inside sceSifRpcLoop and run the stub again once woken.
        m_kernel.sleepCurrent(cpu);
        cpu.pc = stub;
        cpu.branchPending = false;
        return true;
    }

    void IopRpcBridge::onSifTransfer(const SifTransfer &transfer)
    {
        // The EE SIF transport owns the actual directional memory movement.
        // Services still receive both phases through IopSubsystem, but mirroring
        // IOP bytes through an equal-numbered EE address would alias two distinct
        // PS2 address spaces and can overwrite live game data.
        (void)transfer;
    }

    void IopRpcBridge::removeServersInRange(uint32_t base, uint32_t size)
    {
        for (auto server = m_servers.begin(); server != m_servers.end();)
        {
            const uint32_t function = IopMemory::physicalAddress(server->second.function);
            if (function >= base && function < base + size)
                server = m_servers.erase(server);
            else
                ++server;
        }
    }

    bool IopRpcBridge::hasServer(uint32_t sid) const noexcept
    {
        const auto server = m_servers.find(sid);
        return server != m_servers.end() && server->second.function != 0u;
    }
}
