#include "iop_emulator.h"
#include "imports/iop_cdvd.h"
#include "core/iop_cpu.h"
#include "imports/iop_heaplib.h"
#include "imports/iop_imports.h"
#include "imports/iop_intrman.h"
#include "imports/iop_ioman.h"
#include "core/iop_kernel.h"
#include "imports/iop_loadcore.h"
#include "core/iop_memory.h"
#include "services/iop_module_loader.h"
#include "services/iop_rpc.h"
#include "imports/iop_stdio.h"
#include "imports/iop_sysclib.h"
#include "imports/iop_sysmem.h"
#include "imports/iop_timrman.h"
#include "imports/iop_vblank.h"
#include "iop_emulator_const.h"

#include <algorithm>
#include <cstdio>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <cstdlib>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <optional>
#include <span>
#include <sstream>
#include <utility>

namespace ps2x::iop
{
    // [DoD] IOP address breakpoints that only log (set by the host's --iopbp option)
    std::unordered_set<uint32_t> g_dodIopBreakpoints;
    uint32_t g_dodIopBreakpointHits = 0;
    void dodSetIopBreakpoints(const std::vector<uint32_t> &addresses)
    {
        g_dodIopBreakpoints = std::unordered_set<uint32_t>(addresses.begin(), addresses.end());
    }
}

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kRamSize = IopMemory::RamSize;
        constexpr uint32_t kKernelHeapBase = IopMemory::HeapBase;
        constexpr uint32_t kKernelHeapLimit = IopMemory::HeapLimit;
        constexpr uint32_t kCallStackBase = kKernelHeapLimit;
        constexpr uint32_t kCallStackLimit = 0x001FFF00u;
        constexpr uint32_t kCallStackSize = 0x2000u;
        constexpr uint32_t kCallStackCapacity = (kCallStackLimit - kCallStackBase) / kCallStackSize;
        constexpr uint64_t kCdvdCompletionCycles = 128u;
        constexpr int kSpu2Irq = 9;

        uint32_t physicalAddress(uint32_t address)
        {
            return IopMemory::physicalAddress(address);
        }

        int32_t sign16(uint32_t value)
        {
            return static_cast<int16_t>(value & 0xFFFFu);
        }

        bool iequals(std::string_view lhs, std::string_view rhs)
        {
            if (lhs.size() != rhs.size())
                return false;
            for (size_t i = 0; i < lhs.size(); ++i)
            {
                if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
                    std::tolower(static_cast<unsigned char>(rhs[i])))
                    return false;
            }
            return true;
        }

    }

    class IopEmulator::Impl final : public IopGuestExecutor
    {
    public:
        using CpuState = IopCpuState;

        struct Module
        {
            int id = 0;
            std::string path;
            std::string name;
            uint32_t base = 0;
            uint32_t size = 0;
            uint32_t entry = 0;
            uint32_t gp = 0;
            bool resident = false;
        };

        struct GuestCallback
        {
            uint32_t function = 0;
            uint32_t gp = 0;
        };

        struct ScheduledGuestCallback
        {
            uint32_t function = 0u;
            uint32_t gp = 0u;
            uint32_t argument = 0u;
        };

        explicit Impl(IopHost &hostRef)
            : host(hostRef),
              sysmem(host, memory),
              kernel(memory),
              cdvd(host, memory, kernel),
              vblank(kernel),
              rpc(host, memory, kernel),
              sysclib(memory),
              stdio(host, memory),
              heaplib(memory),
              intrman(memory),
              timrman(),
              ioman(memory),
              cpuCore(memory),
              imports(memory),
              loadcore(memory, imports)
        {
            reset();
        }

        void reset()
        {
            memory.reset();
            kernel.reset();
            modules.clear();
            imports.reset();
            rpc.reset();
            cdvd.reset();
            intrman.reset();
            timrman.reset();
            ioman.reset();
            pendingDmaInterrupts.clear();
            pendingGuestCallbacks.clear();
            for (DmacRequest &request : dmacRequests)
                request = {};
            nextModuleId = 1;
            moduleCursor = kModuleLoadBase;
            totalCycles = 0;
            spuCycle = 0;
            totalInstructions = 0;
            eeCycleCarry = 0;
            rpcAheadCycles = 0;
            drivingRpc = false;
            activeCpu = nullptr;
            lastError.clear();
            servicingDmaInterrupts = false;
            servicingGuestCallbacks = false;
            callDepth = 0u;
            secrMcCommandHandler = {};
            secrMcDevIdHandler = {};
            checkKelfPathCallback = {};
        }

        uint8_t read8(uint32_t address) const
        {
            return memory.read8(address);
        }

        uint16_t read16(uint32_t address) const
        {
            return memory.read16(address);
        }

        uint32_t read32(uint32_t address) const
        {
            return memory.read32(address);
        }

        void write8(uint32_t address, uint8_t value)
        {
            memory.write8(address, value);
            schedulePendingDma();
        }

        void write16(uint32_t address, uint16_t value)
        {
            memory.write16(address, value);
            schedulePendingDma();
        }

        void write32(uint32_t address, uint32_t value)
        {
            memory.write32(address, value);
            schedulePendingDma();
        }

        void schedulePendingDma()
        {
            if (const auto dma = memory.takeDmaStart())
                pendingDmaInterrupts[dma->irq] = totalCycles + dma->delayCycles;
            if (memory.takeSio2Interrupt()) // [DoD] SIO2 transfer finished: INTC line 17
                pendingDmaInterrupts[17] = totalCycles + 64u;
        }

        // [DoD] dmacman for the SIO2 channels (11: SIO2in, 12: SIO2out); SIO2MAN programs them with
        // dmac_request(channel, addr, block words, blocks, dir) and starts them with dmac_transfer(channel).
        struct DmacRequest
        {
            uint32_t address = 0;
            uint32_t blockWords = 0;
            uint32_t blocks = 0;
        };
        DmacRequest dmacRequests[16];

        bool dispatchDmacman(uint16_t ordinal, CpuState &cpu)
        {
            const uint32_t channel = cpu.gpr[4];
            switch (ordinal)
            {
            case 28: // dmac_request
                if (channel < 16u)
                    dmacRequests[channel] = {cpu.gpr[5], cpu.gpr[6], cpu.gpr[7]};
                cpu.gpr[2] = 1u;
                return true;
            case 32: // dmac_transfer
                if (channel == 11u || channel == 12u)
                {
                    const DmacRequest &request = dmacRequests[channel];
                    const uint32_t blockBytes = request.blockWords * 4u;
                    const uint32_t total = std::min<uint32_t>(blockBytes * request.blocks, 0x10000u);
                    std::vector<uint8_t> buffer(total);
                    if (channel == 11u)
                    {
                        if (memory.readRam(request.address, buffer.data(), buffer.size()))
                            memory.sio2().dmaIn(buffer.data(), buffer.size(), blockBytes);
                    }
                    else
                    {
                        memory.sio2().dmaOut(buffer.data(), buffer.size());
                        (void)memory.writeRam(request.address, buffer.data(), buffer.size());
                    }
                }
                cpu.gpr[2] = 1u;
                return true;
            default:
                cpu.gpr[2] = 0u;
                return true;
            }
        }

        bool readRam(uint32_t address, void *destination, size_t size) const
        {
            return memory.readRam(address, destination, size);
        }

        bool writeRam(uint32_t address, const void *source, size_t size)
        {
            return memory.writeRam(address, source, size);
        }

        bool zeroRam(uint32_t address, size_t size)
        {
            return memory.zeroRam(address, size);
        }

        bool isHardwareAddress(uint32_t phys) const
        {
            return memory.isHardwareAddress(phys);
        }

        uint32_t allocate(uint32_t size, uint32_t alignment = 16u, std::optional<uint32_t> fixed = std::nullopt)
        {
            return memory.allocate(size, alignment, fixed);
        }

        bool freeAllocation(uint32_t address)
        {
            return memory.freeAllocation(address);
        }

        void log(LogLevel level, std::string_view text)
        {
            host.log(level, text);
        }

        bool checkInterrupt(CpuState &cpu)
        {
            const uint32_t status = cpu.cop0[12];
            if ((status & 1u) == 0u)
                return false;
            if ((status & 0x2u) != 0u)
                return false;
            const bool pending = memory.interruptControl() != 0u && (memory.interruptStatus() & memory.interruptMask()) != 0u;
            if (!pending)
                return false;
            cpu.cop0[13] |= 0x400u;
            cpuCore.raiseException(cpu, 0u, cpu.pc, false);
            return true;
        }

        enum class ImportDisposition
        {
            Handled,
            JumpToGuest,
            Missing,
        };

        ImportDisposition dispatchImport(const IopImportCall &call, CpuState &cpu)
        {
            const uint32_t a0 = cpu.gpr[4];
            auto setV0 = [&](uint32_t value)
            {
                cpu.gpr[2] = value;
            };

            if (iequals(call.library, "sysmem") && sysmem.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            if (iequals(call.library, "cdvdman") && cdvd.dispatchImport(call.ordinal, cpu))
            {
                if (const auto callback = cdvd.takeCompletionCallback())
                {
                    pendingGuestCallbacks.emplace(
                        totalCycles + kCdvdCompletionCycles,
                        ScheduledGuestCallback{
                            callback->address,
                            callback->gp,
                            callback->reason,
                        });
                }
                return ImportDisposition::Handled;
            }

            if (iequals(call.library, "loadcore") && loadcore.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            if (iequals(call.library, "thbase") || iequals(call.library, "threadman"))
            {
                // [DoD] GoW port #130 / KillzoneRecomp findings: DelayThread outside any thread (an RPC server or a
                // module start polling with DelayThread) used to return at once; the IOP now runs for the delay.
                if (call.ordinal == 33u && !kernel.inThread())
                    runOutsideThreadUntil([]() { return false; },
                                          totalCycles + std::max<uint64_t>((static_cast<uint64_t>(cpu.gpr[4]) * 36'864'000ull + 999'999ull) / 1'000'000ull, 1u));
                return kernel.dispatchThreadImport(call.ordinal, cpu, totalCycles)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "thsemap"))
            {
                if (call.ordinal == 8u && !kernel.inThread())
                    waitSemaphoreOutsideThread(static_cast<int>(cpu.gpr[4])); // [DoD] upstream PR #266
                return kernel.dispatchSemaphoreImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "thevent"))
            {
                return kernel.dispatchEventImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "sifcmd"))
            {
                // [DoD] sceSifRpcLoop serves requests on the calling thread (see IopRpcBridge::serviceRpcLoop).
                if (call.ordinal == 22u && rpc.serviceRpcLoop(cpu, kernel.currentThreadId()))
                    return ImportDisposition::JumpToGuest;
                return rpc.dispatchSifCmdImport(call.ordinal, cpu, totalCycles)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "intrman") && intrman.dispatchImport(call.ordinal, cpu, *this))
                return ImportDisposition::Handled;
            if (iequals(call.library, "secrman"))
            {
                switch (call.ordinal)
                {
                case 4: // SecrSetMcCommandHandler
                    secrMcCommandHandler = {a0, cpu.gpr[28]};
                    setV0(0);
                    return ImportDisposition::Handled;
                case 5: // SecrSetMcDevIDHandler
                    secrMcDevIdHandler = {a0, cpu.gpr[28]};
                    setV0(0);
                    return ImportDisposition::Handled;
                default:
                    break;
                }
            }
            if (iequals(call.library, "modload") && call.ordinal == 13u)
            {
                checkKelfPathCallback = {a0, cpu.gpr[28]};
                setV0(0);
                return ImportDisposition::Handled;
            }
            if (iequals(call.library, "ioman") && ioman.dispatchImport(call.ordinal, cpu, *this))
                return ImportDisposition::Handled;
            if (iequals(call.library, "sifman"))
            {
                const uint32_t completion = cpu.gpr[6];
                const uint32_t userdata = cpu.gpr[7];
                const uint32_t callbackGp = cpu.gpr[28];
                if (!rpc.dispatchSifManImport(call.ordinal, cpu))
                    return ImportDisposition::Missing;
                // SDK 2.2+ ordinal 32 transfers exactly like ordinal 7, then
                // invokes completioncb(userdata). The bridge has completed
                // all copies before returning a DMA id. Defer the callback to
                // the executor's next safe boundary, preserving the caller.
                if (call.ordinal == 32u && cpu.gpr[2] != 0u && completion != 0u)
                {
                    pendingGuestCallbacks.emplace(totalCycles,
                        ScheduledGuestCallback{completion, callbackGp, userdata});
                }
                return ImportDisposition::Handled;
            }
            if (iequals(call.library, "vblank") && vblank.dispatchImport(call.ordinal, cpu, totalCycles))
                return ImportDisposition::Handled;
            if (iequals(call.library, "timrman") && timrman.dispatchImport(call.ordinal, cpu, totalCycles))
                return ImportDisposition::Handled;
            if (iequals(call.library, "dmacman"))
            {
                (void)dispatchDmacman(call.ordinal, cpu);
                return ImportDisposition::Handled;
            }
            if (iequals(call.library, "stdio") && stdio.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;
            if (iequals(call.library, "sysclib"))
            {
                return sysclib.dispatchImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "heaplib") && heaplib.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            const uint32_t target = imports.resolve(call.library, call.ordinal, call.version);
            if (target != 0u)
            {
                cpu.pc = target;
                cpu.branchPending = false;
                return ImportDisposition::JumpToGuest;
            }

            std::ostringstream out;
            out << "[IOP] unhandled import " << call.library << ':' << call.ordinal
                << " version=0x" << std::hex << call.version << " pc=0x" << cpu.pc;
            log(LogLevel::Warning, out.str());
            setV0(0);
            return ImportDisposition::Missing;
        }

        bool step(CpuState &cpu)
        {
            if (cpu.stopped)
                return false;
            if (cpu.pc == kThreadReturnSentinel || cpu.pc == kCallReturnSentinel)
            {
                cpu.stopped = true;
                return false;
            }
            if (physicalAddress(cpu.pc) >= kRamSize)
            {
                std::ostringstream out;
                out << "[IOP] execution outside RAM pc=0x" << std::hex << cpu.pc;
                log(LogLevel::Error, out.str());
                cpu.stopped = true;
                return false;
            }
            if (checkInterrupt(cpu))
                return true;

            if (!g_dodIopBreakpoints.empty() && g_dodIopBreakpoints.count(cpu.pc) && g_dodIopBreakpointHits < 400u)
            {
                ++g_dodIopBreakpointHits; // [DoD] diagnostics: --iopbp
                std::fprintf(stderr, "[iopbp] pc=%08X a0=%08X a1=%08X a2=%08X a3=%08X v0=%08X v1=%08X ra=%08X sp=%08X "
                                     "s0=%08X s1=%08X s2=%08X s3=%08X cyc=%llu\n",
                             cpu.pc, cpu.gpr[4], cpu.gpr[5], cpu.gpr[6], cpu.gpr[7], cpu.gpr[2], cpu.gpr[3],
                             cpu.gpr[31], cpu.gpr[29], cpu.gpr[16], cpu.gpr[17], cpu.gpr[18], cpu.gpr[19],
                             static_cast<unsigned long long>(totalCycles));
            }

            if (const auto import = imports.decode(cpu.pc))
            {
                const ImportDisposition disposition = dispatchImport(*import, cpu);
                if (disposition == ImportDisposition::Missing)
                {
                    // [DoD] Most library dispatchers return Missing without a word (only the final fallback logged):
                    // report each missing library:ordinal once (GoW port #129: an unimplemented sceCdStop hung a thread).
                    static std::set<std::pair<std::string, uint32_t>> s_reported;
                    if (s_reported.size() < 256 && s_reported.emplace(import->library, import->ordinal).second)
                    {
                        std::ostringstream out;
                        out << "[IOP] missing import " << import->library << ':' << import->ordinal << " pc=0x" << std::hex
                            << cpu.pc;
                        log(LogLevel::Warning, out.str());
                    }
                }
                ++totalInstructions;
                ++totalCycles;
                if (disposition == ImportDisposition::JumpToGuest)
                    return true;
                cpu.pc = cpu.gpr[31];
                cpu.branchPending = false;
                return !cpu.stopped;
            }

            const bool running = cpuCore.executeInstruction(cpu);
            schedulePendingDma();
            ++totalInstructions;
            ++totalCycles;
            return running;
        }

        uint32_t runCpu(CpuState &cpu, uint32_t instructionBudget)
        {
            CpuState *previous = activeCpu;
            activeCpu = &cpu;
            const uint64_t start = totalInstructions;
            while (!cpu.stopped && !cpu.yielded && totalInstructions - start < instructionBudget)
            {
                if (!step(cpu))
                    break;
                {
                    // [DoD] DOD_IOP_PCHIST=1: sample the IOP pc every 1024 instructions; every 2^27 instructions print
                    // the 30 most frequent pcs (where the interpreter's time goes).
                    static const bool pcHist = std::getenv("DOD_IOP_PCHIST") != nullptr;
                    if (pcHist && (totalInstructions & 1023u) == 0u)
                    {
                        static std::unordered_map<uint32_t, uint32_t> hist;
                        ++hist[cpu.pc & ~0xFFu]; // 256-byte regions
                        if ((totalInstructions & ((1ull << 27) - 1u)) == 0u)
                        {
                            std::vector<std::pair<uint32_t, uint32_t>> rows;
                            uint64_t total = 0;
                            for (const auto &[pc, n] : hist)
                            {
                                rows.emplace_back(n, pc);
                                total += n;
                            }
                            std::sort(rows.rbegin(), rows.rend());
                            std::fprintf(stderr, "[iop-pc] %llu instructions, %llu samples\n",
                                         static_cast<unsigned long long>(totalInstructions), static_cast<unsigned long long>(total));
                            for (size_t i = 0; i < rows.size() && i < 30; ++i)
                                std::fprintf(stderr, "[iop-pc] %6u %5.1f%% %08X\n", rows[i].first, 100.0 * rows[i].first / total, rows[i].second);
                            hist.clear();
                        }
                    }
                }
                // [DoD] Interrupt-context work waits while the thread holds CpuSuspendIntr (I_CTRL = 0), as on the
                // real IOP (Urban Reign port, PROGRESS §34): CRI's ADX driver edits its chunk lists inside such a
                // section and its DMA callback edits the same lists; run in between, a chunk was lost and the CRI
                // stream (and the movie waiting on it) hung.
                if (!interruptsEnabled())
                    continue;
                if (!servicingDmaInterrupts && !pendingDmaInterrupts.empty())
                    servicePendingDmaInterrupts();
                if (!servicingGuestCallbacks && !pendingGuestCallbacks.empty())
                    servicePendingGuestCallbacks();
            }
            activeCpu = previous;
            return static_cast<uint32_t>(totalInstructions - start);
        }

        // [DoD] DOD_IOP_INTR_MASK=0: the old behaviour (interrupt work and thread switches ignore I_CTRL).
        bool interruptsEnabled() const noexcept
        {
            static const bool honour = [] { const char *v = std::getenv("DOD_IOP_INTR_MASK"); return !v || *v != '0'; }();
            return !honour || memory.interruptControl() != 0u;
        }
        int criticalThreadId = 0; // ended its timeslice inside CpuSuspendIntr: runs again before anything else
        // [DoD] DOD_ADMA_WATCH: the thread that ran last and whether it was still ready afterwards.
        IopThread dodPrevRan{};
        bool dodPrevRanReady = false;
        static inline Impl *s_dodWatchImpl = nullptr;
        // [DoD] The thread whose timeslice just ended while it was still ready (IopKernel::beginNextReady's stickyId):
        // it keeps the CPU against ready threads of the same priority, as on the IOP kernel (no timeslices).
        // Upstream picked the lowest id among the highest priority every 256 cycles, so CRI_ADXI's stream thread
        // (prio 39) cut into its mixer thread (prio 39) mid-way through an unlocked SJ update: a stream's right-channel
        // count went off by 3 ADX frames for good, the mixer then read 96 stale samples every 512 (right-channel pops
        // following the music; 2 of 7 runs, 2 of 2 with DOD_IOP_SLICE=64, 0 of 2 with this at DOD_IOP_SLICE=64).
        // DOD_IOP_STICKY=0: upstream behaviour. DOD_IOP_SLICE=<cycles>: the scheduler's slice (default kDefaultSlice),
        // for tests that make same-priority switches more or less frequent.
        int stickyThreadId = 0;
        static bool dodIopSticky()
        {
            static const bool on = [] { const char *v = std::getenv("DOD_IOP_STICKY"); return !(v && v[0] == '0'); }();
            return on;
        }
        static uint32_t dodIopSlice()
        {
            static const uint32_t slice = [] {
                const char *v = std::getenv("DOD_IOP_SLICE");
                const long n = v ? std::strtol(v, nullptr, 0) : 0;
                return n >= 16 ? static_cast<uint32_t>(n) : kDefaultSlice;
            }();
            return slice;
        }

        uint32_t callFunction(uint32_t address,
                              uint32_t a0,
                              uint32_t a1,
                              uint32_t a2,
                              uint32_t a3,
                              uint32_t gp,
                              uint32_t budget = kMaxCallInstructions)
        {
            struct CallDepthGuard
            {
                uint32_t &depth;
                ~CallDepthGuard() { --depth; }
            };

            const uint32_t depth = callDepth++;
            const CallDepthGuard depthGuard{callDepth};
            CpuState cpu{};
            cpu.pc = address;
            cpu.gpr[4] = a0;
            cpu.gpr[5] = a1;
            cpu.gpr[6] = a2;
            cpu.gpr[7] = a3;
            cpu.gpr[28] = gp;
            if (depth < kCallStackCapacity)
            {
                const uint32_t stackTop = kCallStackLimit - depth * kCallStackSize;
                cpu.gpr[29] = stackTop - 32u;
            }
            else if (activeCpu && activeCpu->gpr[29] > kCallStackBase + kStackGuardBytes)
            {
                // Extremely deep re-entrancy borrows unused space below the
                // suspended caller's live frame. Stack growth remains away
                // from the caller, so its saved registers stay intact.
                cpu.gpr[29] = (activeCpu->gpr[29] - kStackGuardBytes) & ~15u;
            }
            else
            {
                cpu.gpr[29] = kCallStackBase - 32u;
            }
            cpu.gpr[31] = kCallReturnSentinel;
            // [DoD] Upstream PR #290: DOD_IOP_CALL_BUDGET overrides the default instruction cap, and a call that runs
            // out of it is reported (the caller got whatever was in v0 and the module was left half-way through).
            static const uint32_t budgetOverride = []() -> uint32_t
            {
                const char *v = std::getenv("DOD_IOP_CALL_BUDGET");
                return v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 0)) : 0u;
            }();
            if (budget == kMaxCallInstructions && budgetOverride != 0u)
                budget = budgetOverride;
            runCpu(cpu, budget);
            if (!cpu.stopped && !cpu.yielded)
            {
                std::ostringstream out;
                out << "[IOP:call-abandoned] fn=0x" << std::hex << address << " stoppedAt=0x" << cpu.pc << " v0=0x"
                    << cpu.gpr[2] << std::dec << " budget=" << budget;
                log(LogLevel::Warning, out.str());
            }
            return cpu.gpr[2];
        }

        uint32_t executeGuestFunction(uint32_t address,
                                      uint32_t a0,
                                      uint32_t a1,
                                      uint32_t a2,
                                      uint32_t a3,
                                      uint32_t gp) override
        {
            return callFunction(address, a0, a1, a2, a3, gp);
        }

        uint32_t executeGuestFunctionWithBudget(uint32_t address,
                                                uint32_t a0,
                                                uint32_t a1,
                                                uint32_t a2,
                                                uint32_t a3,
                                                uint32_t gp,
                                                uint32_t instructionBudget) override
        {
            return callFunction(address, a0, a1, a2, a3, gp, instructionBudget);
        }

        // Not that good to use exception handling for control flow but will do for now
        void servicePendingDmaInterrupts()
        {
            if (servicingDmaInterrupts || pendingDmaInterrupts.empty())
                return;

            servicingDmaInterrupts = true;

            std::vector<int> completed;
            for (auto it = pendingDmaInterrupts.begin(); it != pendingDmaInterrupts.end();)
            {
                if (it->second > totalCycles)
                {
                    ++it;
                    continue;
                }
                completed.push_back(it->first);
                it = pendingDmaInterrupts.erase(it);
            }
            try
            {
                for (const int irq : completed)
                    (void)intrman.dispatchInterrupt(irq, *this);
            }
            catch (...)
            {
                servicingDmaInterrupts = false;
                throw;
            }
            servicingDmaInterrupts = false;
        }

        void servicePendingGuestCallbacks()
        {
            if (servicingGuestCallbacks || pendingGuestCallbacks.empty())
                return;

            std::vector<ScheduledGuestCallback> callbacks;
            for (auto it = pendingGuestCallbacks.begin(); it != pendingGuestCallbacks.end();)
            {
                if (it->first > totalCycles)
                    break;
                callbacks.push_back(it->second);
                it = pendingGuestCallbacks.erase(it);
            }
            if (callbacks.empty())
                return;

            servicingGuestCallbacks = true;
            try
            {
                for (const ScheduledGuestCallback &callback : callbacks)
                {
                    if (callback.function != 0u)
                    {
                        (void)callFunction(callback.function,
                                           callback.argument,
                                           0u,
                                           0u,
                                           0u,
                                           callback.gp,
                                           100000u);
                    }
                }
            }
            catch (...)
            {
                servicingGuestCallbacks = false;
                throw;
            }
            servicingGuestCallbacks = false;
        }

        // [DoD] Idle fast path (KillzoneRecomp 0012): the earliest IOP cycle at which anything can happen while every
        // thread waits (thread wake, DMA/callback completion, timer), valid until something outside touches the IOP
        // (every IopEmulator entry point clears it). runCycles is called on nearly every EE checkpoint with a few
        // cycles; while the IOP sleeps those calls only advance the clock. 0 = unknown. DOD_IOP_IDLE_FASTPATH=0: off.
        uint64_t idleUntil = 0;

        // Runs the SPU2 up to the current IOP time (ran-j/PS2Recomp#308), hands its output to the host and raises the
        // SPU2 interrupt when an IRQA address was hit. [DoD] With AutoDMA it also ends DMA 4/7 when the SPU2 has
        // pulled the block's last bytes and stops there, so LIBSD's interrupt handler queues the next block before the
        // SPU2 needs it. True when it queued an interrupt.
        bool advanceSpu()
        {
            if (!IopMemory::spu2Enabled())
                return false;
            constexpr uint64_t kCyclesPerSample = IopSpu2::IopCyclesPerSample;
            constexpr uint32_t kChunkFrames = 256u;
            constexpr int kDmaSpuIrq[2] = {0x24, 0x28};
            IopSpu2 &spu = memory.spu2();
            bool raised = false;
            while (spuCycle + kCyclesPerSample <= totalCycles)
            {
                const uint32_t frames = static_cast<uint32_t>(
                    std::min<uint64_t>((totalCycles - spuCycle) / kCyclesPerSample, kChunkFrames));
                spuBuffer.resize(static_cast<size_t>(frames) * 2u);
                const uint32_t produced = spu.generate(spuBuffer.data(), frames);
                spuCycle += produced * kCyclesPerSample;
                host.audioOutput(spuBuffer.data(), produced);
                if (spu.takeIrq())
                {
                    pendingDmaInterrupts[kSpu2Irq] = spuCycle;
                    raised = true;
                }
                bool autoDmaEnded = false;
                for (int core = 0; core < 2; ++core)
                {
                    if (spu.takeAutoDmaDone(core))
                    {
                        pendingDmaInterrupts[kDmaSpuIrq[core]] = spuCycle;
                        autoDmaEnded = true;
                    }
                }
                if (autoDmaEnded)
                    return true;
            }
            return raised;
        }

        void runCycles(uint64_t cycles) noexcept
        {
            static const bool fastPath = []() {
                const char *v = std::getenv("DOD_IOP_IDLE_FASTPATH");
                return !(v && v[0] == '0');
            }();
            if (fastPath && idleUntil != 0 && totalCycles + cycles < idleUntil)
            {
                totalCycles += cycles; // exactly what the idle branch below would do
                // [DoD] The SPU2 keeps playing while the IOP idles; an interrupt it raises ends the idle stretch.
                if (advanceSpu())
                    idleUntil = 0;
                return;
            }
            idleUntil = 0;
            try
            {
                const uint64_t target = totalCycles + cycles;
                while (totalCycles < target)
                {
                    advanceSpu();
                    // [DoD] A thread whose timeslice ended inside CpuSuspendIntr keeps the CPU: no interrupt work and
                    // no thread switch until it re-enables interrupts or blocks (Urban Reign port, PROGRESS §34).
                    IopThread *next = nullptr;
                    if (criticalThreadId != 0)
                    {
                        const IopThread *held = kernel.findThread(criticalThreadId);
                        if (!interruptsEnabled() && held && held->state == IopThreadState::Ready)
                            next = kernel.beginThread(criticalThreadId);
                        criticalThreadId = 0;
                    }
                    if (next == nullptr)
                    {
                        servicePendingDmaInterrupts();
                        servicePendingGuestCallbacks();
                        timrman.serviceDue(totalCycles, *this);
                        next = kernel.beginNextReady(totalCycles, dodIopSticky() ? stickyThreadId : 0);
                    }
                    if (!next)
                    {
                        uint64_t nextWake = kernel.nextWakeCycle(UINT64_MAX);
                        for (const auto &[irq, completionCycle] : pendingDmaInterrupts)
                            nextWake = std::min(nextWake, completionCycle);
                        if (!pendingGuestCallbacks.empty())
                            nextWake = std::min(nextWake, pendingGuestCallbacks.begin()->first);
                        nextWake = timrman.nextEventCycle(nextWake);
                        idleUntil = nextWake > totalCycles ? nextWake : 0;
                        totalCycles = std::max(totalCycles + 1u, std::min(target, nextWake));
                        continue;
                    }
                    idleUntil = 0;
                    // [DoD] DOD_ADMA_WATCH (diagnostics): note a switch away from a thread that was still ready.
                    if (g_dodAdmaLastWrite)
                    {
                        if (!g_dodDescribeIopThreads)
                        {
                            s_dodWatchImpl = this;
                            g_dodDescribeIopThreads = []() {
                                for (const std::string &line : s_dodWatchImpl->kernel.describeThreads())
                                    std::fprintf(stderr, "[adma-sched] %s\n", line.c_str());
                            };
                        }
                        if (dodPrevRanReady && next->id != dodPrevRan.id)
                        {
                            DodSchedEvent &event = g_dodSchedRing[g_dodSchedCount++ % 256u];
                            event = DodSchedEvent{g_dodAdmaNow, dodPrevRan.id, static_cast<int>(dodPrevRan.priority), dodPrevRan.cpu.pc,
                                                  next->id, static_cast<int>(next->priority), next->cpu.pc};
                        }
                    }
                    const uint64_t before = totalCycles;
                    runCpu(next->cpu, static_cast<uint32_t>(std::min<uint64_t>(dodIopSlice(), target - totalCycles)));
                    const int ranId = next->id;
                    kernel.endTimeslice(*next, kThreadReturnSentinel);
                    const IopThread *ranAfter = kernel.findThread(ranId); // endTimeslice may have removed a dead thread
                    stickyThreadId = ranAfter && ranAfter->state == IopThreadState::Ready ? ranId : 0;
                    if (g_dodAdmaLastWrite)
                    {
                        dodPrevRanReady = stickyThreadId != 0;
                        if (ranAfter)
                        {
                            dodPrevRan.id = ranAfter->id;
                            dodPrevRan.priority = ranAfter->priority;
                            dodPrevRan.cpu.pc = ranAfter->cpu.pc;
                        }
                    }
                    if (!interruptsEnabled())
                    {
                        if (const IopThread *ran = kernel.findThread(ranId); ran && ran->state == IopThreadState::Ready)
                            criticalThreadId = ranId;
                    }
                    if (totalCycles == before)
                        ++totalCycles;
                }
            }
            catch (...)
            {
                // Runtime scheduling must never throw through EeScheduler::accountCycles().
            }
        }

        // [DoD] Upstream PR #266: RPC server functions and module start routines run synchronously (callFunction),
        // outside any IOP thread. A WaitSema there that finds the count at 0 cannot block, so the caller went on
        // without the semaphore while the IOP thread holding it was preempted inside its critical section (Killzone:
        // a streaming request was dropped and the EE waited forever). On a real IOP the caller blocks and the holder
        // runs until it signals: run the IOP scheduler (threads, due interrupts, callbacks, timers) until the
        // semaphore is signalled, then let WaitSema take it. Not from an interrupt handler or guest callback, nor
        // while the scheduler is already running; gives up after one second of IOP time.
        bool inOutsideSemaphoreWait = false;
        void waitSemaphoreOutsideThread(int id)
        {
            static const bool enabled = [] { const char *v = std::getenv("DOD_IOP_SEMA_OUTSIDE_WAIT"); return !v || *v != '0'; }();
            if (!enabled || kernel.semaphoreCount(id) != 0)
                return; // available, or an unknown id (WaitSema reports it)
            if (inOutsideSemaphoreWait || servicingDmaInterrupts || servicingGuestCallbacks)
                return;
            kernel.setOutsideSemaphoreWait(id);
            try
            {
                runOutsideThreadUntil([&]() { return kernel.semaphoreCount(id) != 0; }, totalCycles + 36'864'000ull);
            }
            catch (...)
            {
                kernel.setOutsideSemaphoreWait(0);
                throw;
            }
            kernel.setOutsideSemaphoreWait(0);
            if (kernel.semaphoreCount(id) == 0)
            {
                std::ostringstream out;
                out << "[IOP] WaitSema(" << id << ") outside a thread timed out";
                log(LogLevel::Warning, out.str());
            }
            else
            {
                static int s_logged = 0;
                if (s_logged < 16)
                {
                    ++s_logged;
                    std::ostringstream out;
                    out << "[IOP] WaitSema(" << id << ") outside a thread waited for its holder";
                    log(LogLevel::Info, out.str());
                }
            }
        }

        // [DoD] Runs the IOP scheduler (threads, due interrupts, callbacks, timers) from code that runs outside any
        // IOP thread, until done() or the cycle limit. Not re-entered, nor from an interrupt handler or callback.
        template <typename Done>
        void runOutsideThreadUntil(Done done, uint64_t limit)
        {
            if (inOutsideSemaphoreWait || servicingDmaInterrupts || servicingGuestCallbacks)
                return;
            inOutsideSemaphoreWait = true;
            idleUntil = 0;
            try
            {
                while (!done() && totalCycles < limit)
                {
                    advanceSpu();
                    servicePendingDmaInterrupts();
                    servicePendingGuestCallbacks();
                    timrman.serviceDue(totalCycles, *this);
                    IopThread *next = kernel.beginNextReady(totalCycles, dodIopSticky() ? stickyThreadId : 0);
                    if (!next)
                    {
                        uint64_t nextWake = kernel.nextWakeCycle(limit);
                        for (const auto &[irq, completionCycle] : pendingDmaInterrupts)
                            nextWake = std::min(nextWake, completionCycle);
                        if (!pendingGuestCallbacks.empty())
                            nextWake = std::min(nextWake, pendingGuestCallbacks.begin()->first);
                        nextWake = timrman.nextEventCycle(nextWake);
                        totalCycles = std::max(totalCycles + 1u, std::min(limit, nextWake));
                        continue;
                    }
                    CpuState *outer = activeCpu;
                    const uint64_t before = totalCycles;
                    runCpu(next->cpu, static_cast<uint32_t>(std::min<uint64_t>(dodIopSlice(), limit - totalCycles)));
                    activeCpu = outer;
                    const int ranId = next->id;
                    kernel.endTimeslice(*next, kThreadReturnSentinel);
                    const IopThread *ranAfter = kernel.findThread(ranId);
                    stickyThreadId = ranAfter && ranAfter->state == IopThreadState::Ready ? ranId : 0;
                    if (totalCycles == before)
                        ++totalCycles;
                }
            }
            catch (...)
            {
                inOutsideSemaphoreWait = false;
                throw;
            }
            inOutsideSemaphoreWait = false;
            idleUntil = 0;
        }

        // [DoD] An EE sceSifCallRpc to a server whose thread sits in sceSifRpcLoop: queue the request, wake that thread
        // and run the IOP until the server function has returned (the EE caller waits for the reply on hardware too).
        // The IOP cycles spent here are taken back from the next EE-driven advances so both clocks stay in step.
        RpcResult handleRpcRequest(const RpcRequest &request)
        {
            if (activeCpu == nullptr && !drivingRpc)
            {
                if (const uint64_t id = rpc.queueRequest(request))
                {
                    constexpr uint64_t kSlice = 2048u;
                    constexpr uint64_t kLimit = 8u * 1024u * 1024u; // ~0.23 s of IOP time
                    drivingRpc = true;
                    const uint64_t start = totalCycles;
                    while (!rpc.isRequestComplete(id) && totalCycles - start < kLimit)
                        runCycles(kSlice);
                    drivingRpc = false;
                    rpcAheadCycles += totalCycles - start;
                    const bool complete = rpc.isRequestComplete(id);
                    rpc.forgetRequest(id);
                    if (!complete)
                    {
                        std::ostringstream out;
                        out << "[IOP] RPC sid=0x" << std::hex << request.sid << " fno=0x" << request.function
                            << " still running after " << std::dec << kLimit << " IOP cycles";
                        log(LogLevel::Warning, out.str());
                    }
                    RpcResult result{};
                    result.handled = true;
                    result.resultAddress = request.receive.address;
                    result.serverDispatchPolicy = ServerDispatchPolicy::Suppress;
                    result.signalNowaitCompletion = true;
                    result.signalCompletion = true;
                    return result;
                }
            }
            return rpc.handleRpc(request, *this);
        }

        ModuleLoadResult loadImage(std::string path, std::span<const uint8_t> image, const void *arguments, uint32_t argumentSize)
        {
            ModuleLoadResult result{true, -1, -1};
            const IopImageLoadResult loaded = IopModuleLoader::load(image, memory, moduleCursor);
            moduleCursor = loaded.nextModuleCursor;
            if (!loaded)
            {
                if (loaded.error == IopImageLoadError::InvalidElf)
                    log(LogLevel::Error, "[IOP] rejected invalid/non-MIPS IRX ELF");
                else if (loaded.error == IopImageLoadError::ArenaExhausted)
                    log(LogLevel::Error, "[IOP] module arena exhausted");
                return result;
            }
            if (!loaded.relocationsComplete)
                log(LogLevel::Warning, "[IOP] one or more IRX relocations were unsupported");

            Module module;
            module.id = nextModuleId++;
            module.path = std::move(path);
            const size_t slash = module.path.find_last_of("/\\:");
            module.name = slash == std::string::npos ? module.path : module.path.substr(slash + 1u);
            module.base = loaded.base;
            module.size = loaded.size;
            module.entry = loaded.entry;
            module.gp = loaded.gp;

            // [DoD] Like the IOP's loadcore, start the module as start(argc, argv): argv[0] is the module path and the
            // NUL-separated argument block supplies argv[1..]. (Capcom's SNDDRV.IRX checks argc == 11 and reads argv[1..10].)
            std::vector<std::string> argvStrings;
            argvStrings.push_back(module.path);
            if (arguments && argumentSize)
            {
                const char *text = static_cast<const char *>(arguments);
                uint32_t begin = 0u;
                for (uint32_t i = 0u; i <= argumentSize; ++i)
                {
                    if (i == argumentSize || text[i] == '\0')
                    {
                        if (i > begin || i < argumentSize)
                            argvStrings.emplace_back(text + begin, i - begin);
                        begin = i + 1u;
                    }
                }
                while (argvStrings.size() > 1u && argvStrings.back().empty())
                    argvStrings.pop_back();
            }
            const uint32_t argc = static_cast<uint32_t>(argvStrings.size());
            uint32_t blockSize = (argc + 1u) * 4u;
            for (const std::string &s : argvStrings)
                blockSize += static_cast<uint32_t>(s.size()) + 1u;
            uint32_t args = allocate(blockSize, 16u);
            if (args)
            {
                uint32_t strings = args + (argc + 1u) * 4u;
                for (uint32_t i = 0u; i < argc; ++i)
                {
                    const uint32_t pointer = strings;
                    writeRam(args + i * 4u, &pointer, 4u);
                    writeRam(strings, argvStrings[i].data(), static_cast<uint32_t>(argvStrings[i].size()));
                    write8(strings + static_cast<uint32_t>(argvStrings[i].size()), 0u);
                    strings += static_cast<uint32_t>(argvStrings[i].size()) + 1u;
                }
                const uint32_t terminator = 0u;
                writeRam(args + argc * 4u, &terminator, 4u);
            }
            const uint32_t startResult = callFunction(module.entry, args ? argc : 0u, args, 0u, 0u, module.gp);
            if (args)
                freeAllocation(args);
            module.resident = startResult == 0u || startResult == 2u;
            result.moduleId = module.id;
            result.startResult = static_cast<int32_t>(startResult);
            modules[module.id] = std::move(module);

            std::ostringstream out;
            out << "[IOP] loaded IRX id=" << result.moduleId
                << " entry=0x" << std::hex << modules[result.moduleId].entry
                << " base=0x" << modules[result.moduleId].base
                << " start=" << std::dec << result.startResult
                << " argc=" << argc;
            for (uint32_t i = 1u; i < argc; ++i)
                out << " [" << argvStrings[i] << "]";
            log(LogLevel::Info, out.str());
            return result;
        }

        ModuleLoadResult loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
        {
            std::vector<uint8_t> image;
            if (!IopModuleLoader::readWholeHostFile(host, path, image))
            {
                log(LogLevel::Warning, std::string("[IOP] failed to open IRX '") + std::string(path) + "'");
                return {true, -1, -1};
            }
            return loadImage(std::string(path), image, arguments, argumentSize);
        }

        ModuleLoadResult loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
        {
            std::vector<uint8_t> image;
            if (!IopModuleLoader::readElfFromGuest(host, guestAddress, image))
                return {true, -1, -1};
            std::ostringstream tag;
            tag << "buffer@0x" << std::hex << guestAddress;
            return loadImage(tag.str(), image, arguments, argumentSize);
        }

        bool stopModule(int32_t moduleId, int32_t *result)
        {
            auto it = modules.find(moduleId);
            if (it == modules.end())
                return false;
            // A removable IRX normally exposes a stop entry through module metadata. We do not guess it; terminate owned execution and release the image cleanly.
            kernel.terminateThreadsInRange(it->second.base, it->second.size);
            rpc.removeServersInRange(it->second.base, it->second.size);
            imports.eraseRange(it->second.base, it->second.size);
            modules.erase(it);
            kernel.cleanupDeadThreads();
            if (result)
                *result = 0;
            return true;
        }

        IopHost &host;
        IopMemory memory;
        IopSysmem sysmem;
        IopKernel kernel;
        IopCdvd cdvd;
        IopVblank vblank;
        IopRpcBridge rpc;
        IopSysclib sysclib;
        IopStdio stdio;
        IopHeaplib heaplib;
        IopIntrman intrman;
        IopTimrman timrman;
        IopIoman ioman;
        IopCpuCore cpuCore;
        IopImportRegistry imports;
        IopLoadcore loadcore;
        std::map<int, Module> modules;
        std::map<int, uint64_t> pendingDmaInterrupts;
        std::multimap<uint64_t, ScheduledGuestCallback> pendingGuestCallbacks;
        uint32_t nextModuleId = 1;
        uint32_t moduleCursor = kModuleLoadBase;
        uint64_t totalCycles = 0;
        uint64_t spuCycle = 0;
        std::vector<int16_t> spuBuffer;
        uint64_t totalInstructions = 0;
        uint64_t eeCycleCarry = 0;
        uint64_t rpcAheadCycles = 0; // [DoD] IOP cycles already run while an EE RPC waited
        bool drivingRpc = false;
        CpuState *activeCpu = nullptr;
        std::string lastError;
        bool servicingDmaInterrupts = false;
        bool servicingGuestCallbacks = false;
        uint32_t callDepth = 0u;
        GuestCallback secrMcCommandHandler;
        GuestCallback secrMcDevIdHandler;
        GuestCallback checkKelfPathCallback;
    };

    IopEmulator::IopEmulator(IopHost &host)
        : m_impl(std::make_unique<Impl>(host))
    {
    }

    IopEmulator::~IopEmulator() = default;

    void IopEmulator::reset()
    {
        m_impl->idleUntil = 0;
        m_impl->reset();
    }

    ModuleLoadResult IopEmulator::loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
    {
        m_impl->idleUntil = 0;
        return m_impl->loadModule(path, arguments, argumentSize);
    }

    ModuleLoadResult IopEmulator::loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
    {
        m_impl->idleUntil = 0;
        return m_impl->loadModuleBuffer(guestAddress, arguments, argumentSize);
    }

    bool IopEmulator::stopModule(int32_t moduleId, int32_t *result)
    {
        m_impl->idleUntil = 0;
        return m_impl->stopModule(moduleId, result);
    }

    void IopEmulator::runEeCycles(uint64_t eeCycles) noexcept
    {
        const uint64_t total = m_impl->eeCycleCarry + eeCycles;
        uint64_t iopCycles = total / 8u;
        m_impl->eeCycleCarry = total % 8u;
        const uint64_t ahead = std::min(iopCycles, m_impl->rpcAheadCycles);
        m_impl->rpcAheadCycles -= ahead;
        iopCycles -= ahead;
        if (iopCycles)
            m_impl->runCycles(iopCycles);
    }

    RpcResult IopEmulator::handleRpc(const RpcRequest &request)
    {
        m_impl->idleUntil = 0;
        return m_impl->handleRpcRequest(request);
    }

    bool IopEmulator::hasRpcServer(uint32_t sid) const noexcept
    {
        return m_impl->rpc.hasServer(sid);
    }

    void IopEmulator::onSifTransfer(const SifTransfer &transfer)
    {
        m_impl->idleUntil = 0;
        m_impl->rpc.onSifTransfer(transfer);
    }

    void IopEmulator::changeMedia()
    {
        m_impl->cdvd.changeMedia();
    }

    uint32_t IopEmulator::allocateMemory(uint32_t size, uint32_t alignment)
    {
        m_impl->idleUntil = 0;
        return m_impl->memory.allocate(size, alignment);
    }

    bool IopEmulator::freeMemory(uint32_t address)
    {
        m_impl->idleUntil = 0;
        return m_impl->memory.freeAllocation(address);
    }

    bool IopEmulator::readMemory(uint32_t address, void *destination, size_t size) const
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.readRam(address, destination, size);
    }

    bool IopEmulator::writeMemory(uint32_t address, const void *source, size_t size)
    {
        m_impl->idleUntil = 0;
        return isMemoryRange(address, size) &&
               m_impl->memory.writeRam(address, source, size);
    }

    bool IopEmulator::zeroMemory(uint32_t address, size_t size)
    {
        m_impl->idleUntil = 0;
        return isMemoryRange(address, size) &&
               m_impl->memory.zeroRam(address, size);
    }

    bool IopEmulator::isMemoryRange(uint32_t address, size_t size) const
    {
        const bool physicalSegment = address < IopMemory::RamSize;
        const bool cachedSegment = address >= 0x80000000u && address < 0x80200000u;
        const bool uncachedSegment = address >= 0xA0000000u && address < 0xA0200000u;
        if (!physicalSegment && !cachedSegment && !uncachedSegment)
            return false;
        const uint32_t physical = IopMemory::physicalAddress(address);
        return physical <= IopMemory::RamSize && size <= IopMemory::RamSize - physical;
    }

    uint64_t IopEmulator::cycles() const noexcept
    {
        return m_impl->totalCycles;
    }

    uint64_t IopEmulator::instructions() const noexcept
    {
        return m_impl->totalInstructions;
    }

    uint32_t IopEmulator::loadedModuleCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->modules.size());
    }

    std::vector<std::string> IopEmulator::describeThreads() const
    {
        return m_impl->kernel.describeThreads();
    }

    uint32_t IopEmulator::threadCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->kernel.threadCount());
    }

    uint32_t IopEmulator::rpcServerCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->rpc.serverCount());
    }

}
