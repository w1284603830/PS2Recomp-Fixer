#include "iop_compat_test_support.h"
#include "emulator/core/iop_kernel.h"
#include "emulator/core/iop_memory.h"

// Behavioral contracts from ps2dev/ps2sdk thmsgbx.h, thmsgbx.c and kerr.h, reviewed at
// f1ca58a468ead937155ab818d9a38e52c8adf861. No game, sound-driver address or RPC response is substituted.
namespace
{
    using namespace iop_test;
    using namespace ps2x::iop::detail;

    struct Fixture
    {
        IopMemory memory;
        IopKernel kernel{memory};

        int call(uint16_t ordinal, uint32_t a0, uint32_t a1 = 0u, bool interrupt = false)
        {
            IopCpuState cpu{};
            cpu.gpr[4] = a0;
            cpu.gpr[5] = a1;
            require(kernel.dispatchMailboxImport(ordinal, cpu, interrupt), "mailbox ordinal unhandled");
            return static_cast<int32_t>(cpu.gpr[2]);
        }
        int create(uint32_t attr = 0u)
        {
            memory.write32(0x1000u, attr);
            memory.write32(0x1004u, 0x12345678u);
            return call(4u, 0x1000u);
        }
        void packet(uint32_t address, uint8_t priority = 0u)
        {
            memory.write32(address, 0xCAFEBABEu);
            memory.write32(address + 4u, priority);
        }
        int thread(uint32_t priority)
        {
            for (uint32_t offset = 0u; offset < 20u; offset += 4u)
                memory.write32(0x1100u + offset, 0u);
            memory.write32(0x1108u, 0x4000u);
            memory.write32(0x110Cu, 0x400u);
            memory.write32(0x1110u, priority);
            IopCpuState cpu{};
            cpu.gpr[4] = 0x1100u;
            require(kernel.dispatchThreadImport(4u, cpu, 0u), "CreateThread unhandled");
            const int id = static_cast<int>(cpu.gpr[2]);
            require(id > 0, "CreateThread failed");
            cpu.gpr[4] = static_cast<uint32_t>(id);
            require(kernel.dispatchThreadImport(6u, cpu, 0u) && cpu.gpr[2] == 0u, "StartThread failed");
            return id;
        }
        void block(int threadId, int mailbox, uint32_t slot)
        {
            IopThread *t = kernel.beginThread(threadId);
            require(t != nullptr, "receiver thread missing");
            memory.write32(slot, 0xDEADBEEFu);
            t->cpu.gpr[4] = slot;
            t->cpu.gpr[5] = static_cast<uint32_t>(mailbox);
            require(kernel.dispatchMailboxImport(8u, t->cpu), "ReceiveMbx unhandled");
            require(t->state == IopThreadState::Mailbox && t->cpu.yielded, "empty Receive did not block");
            kernel.endTimeslice(*t, 0xFFFFFFFFu);
        }
    };

    void fifoAndAbi()
    {
        Fixture f;
        const int box = f.create();
        require(box > 0, "CreateMbx failed");
        f.packet(0x2000u, 9u);
        f.packet(0x2100u, 1u);
        require(f.call(6u, box, 0x2000u) == 0 && f.call(6u, box, 0x2100u) == 0, "SendMbx failed");
        require(f.memory.read32(0x2000u) == 0x2100u && f.memory.read32(0x2100u) == 0x2000u,
                "queued packets do not expose the IOP circular next links");
        require(f.call(9u, 0x3000u, box) == 0 && f.memory.read32(0x3000u) == 0x2000u,
                "PollMbx argument order or FIFO delivery wrong");
        require(f.memory.read32(0x2100u) == 0x2100u, "remaining one-message ring was not repaired");
        require(f.call(8u, 0x3000u, box) == 0 && f.memory.read32(0x3000u) == 0x2100u,
                "queued ReceiveMbx did not consume the message");
        require(f.call(9u, 0x3000u, box) == -424 && f.memory.read32(0x3000u) == 0x2100u,
                "empty PollMbx must return KE_MBOX_NOMSG without writing msgvar");
        require(f.call(8u, 0x3000u, box) == -417, "outside empty Receive falsely succeeded");
    }

    void messagePriorityAndStatus()
    {
        Fixture f;
        const int box = f.create(4u);
        f.packet(0x2000u, 5u); f.packet(0x2100u, 2u); f.packet(0x2200u, 2u);
        for (uint32_t packet : {0x2000u, 0x2100u, 0x2200u})
            require(f.call(6u, box, packet) == 0, "priority Send failed");
        f.memory.write32(0x3114u, 0xAAAAAAAAu); f.memory.write32(0x3118u, 0xBBBBBBBBu);
        require(f.call(11u, box, 0x3100u) == 0, "ReferMbxStatus failed");
        require(f.memory.read32(0x3100u) == 4u && f.memory.read32(0x3104u) == 0x12345678u &&
                f.memory.read32(0x3108u) == 0u && f.memory.read32(0x310Cu) == 3u &&
                f.memory.read32(0x3110u) == 0x2100u, "status ABI fields wrong");
        require(f.memory.read32(0x3114u) == 0xAAAAAAAAu && f.memory.read32(0x3118u) == 0xBBBBBBBBu,
                "status wrote reserved words");
        for (uint32_t expected : {0x2100u, 0x2200u, 0x2000u})
            require(f.call(9u, 0x3000u, box) == 0 && f.memory.read32(0x3000u) == expected,
                    "MBA_MSPRI is not stable ascending message priority");
    }

    void receiverOrdering()
    {
        for (uint32_t attr : {0u, 1u})
        {
            Fixture f;
            const int box = f.create(attr);
            const int high = f.thread(10u), low = f.thread(50u);
            f.block(low, box, 0x3000u);
            f.block(high, box, 0x3004u);
            require(f.call(11u, box, 0x3100u) == 0 && f.memory.read32(0x3108u) == 2u,
                    "blocked receivers missing from mailbox status");
            IopCpuState status{};
            status.gpr[4] = high; status.gpr[5] = 0x3200u;
            require(f.kernel.dispatchThreadImport(22u, status, 0u) && f.memory.read32(0x3224u) == 5u,
                    "thread waitType is not TSW_MBX");
            f.packet(0x2000u);
            require(f.call(6u, box, 0x2000u) == 0, "direct Send failed");
            require(f.memory.read32(attr == 0u ? 0x3000u : 0x3004u) == 0x2000u,
                    "MBA_THFIFO/THPRI selected the wrong receiver");
            require(f.memory.read32(0x2000u) == 0xCAFEBABEu, "direct delivery touched packet.next");
            require(f.kernel.mailboxMessageCount(box) == 0, "direct delivery was also queued");
        }
        Fixture f;
        const int box = f.create(1u), first = f.thread(30u), second = f.thread(30u);
        f.block(second, box, 0x3004u); f.block(first, box, 0x3000u);
        f.packet(0x2000u);
        require(f.call(6u, box, 0x2000u) == 0 && f.memory.read32(0x3004u) == 0x2000u,
                "equal-priority receivers did not retain arrival order");
    }

    void deleteReleaseAndSuspend()
    {
        Fixture f;
        const int box = f.create(), a = f.thread(20u), b = f.thread(30u);
        f.block(a, box, 0x3000u); f.block(b, box, 0x3004u);
        IopCpuState cpu{}; cpu.gpr[4] = b;
        require(f.kernel.dispatchThreadImport(29u, cpu, 0u), "SuspendThread unhandled");
        require(f.kernel.findThread(b)->state == IopThreadState::MailboxSuspended, "suspension lost mailbox wait");
        require(f.call(5u, box) == 0, "DeleteMbx failed");
        require(f.kernel.findThread(a)->state == IopThreadState::Ready &&
                f.kernel.findThread(b)->state == IopThreadState::Suspended &&
                static_cast<int32_t>(f.kernel.findThread(a)->cpu.gpr[2]) == -425 &&
                static_cast<int32_t>(f.kernel.findThread(b)->cpu.gpr[2]) == -425,
                "deletion failed to wake/cancel both normal and suspended waits");
        require(f.memory.read32(0x3000u) == 0xDEADBEEFu && f.memory.read32(0x3004u) == 0xDEADBEEFu,
                "DeleteMbx wrote a phantom packet");
        const int next = f.create();
        f.block(a, next, 0x3000u);
        cpu.gpr[4] = a;
        require(f.kernel.dispatchThreadImport(18u, cpu, 0u), "ReleaseWaitThread unhandled");
        require(static_cast<int32_t>(f.kernel.findThread(a)->cpu.gpr[2]) == -418,
                "mailbox ReleaseWaitThread did not return KE_RELEASE_WAIT");
        f.packet(0x2000u);
        require(f.call(6u, next, 0x2000u) == 0 && f.kernel.mailboxMessageCount(next) == 1,
                "released receiver stole a later message");
        f.kernel.reset();
        require(f.call(5u, next) == -410, "kernel reset retained mailbox");
    }

    void interruptsAndPreemption()
    {
        Fixture f;
        const int box = f.create(), receiver = f.thread(10u), producer = f.thread(50u);
        f.block(receiver, box, 0x3000u);
        IopThread *sender = f.kernel.beginThread(producer);
        require(sender != nullptr, "producer missing");
        f.packet(0x2000u);
        IopCpuState interrupt{}; interrupt.gpr[4] = box; interrupt.gpr[5] = 0x2000u;
        require(f.kernel.dispatchMailboxImport(7u, interrupt, true) && interrupt.gpr[2] == 0u,
                "iSendMbx failed from interrupt context");
        require(sender->cpu.yielded && !interrupt.yielded && f.kernel.findThread(receiver)->state == IopThreadState::Ready,
                "higher-priority receiver did not preempt after the callback returns");
        f.kernel.endTimeslice(*sender, 0xFFFFFFFFu);
        require(f.call(7u, box, 0x2000u) == -100, "iSendMbx allowed thread context");
        require(f.call(6u, box, 0x2000u, true) == -100, "SendMbx allowed interrupt context");
        require(f.call(8u, 0x3004u, box, true) == -100, "ReceiveMbx allowed interrupt context");
        require(f.call(12u, box, 0x3100u, true) == 0 && f.call(12u, box, 0x3100u) == -100,
                "iReferMbxStatus context rules wrong");
        require(f.create(2u) == -401 && f.call(6u, 999u, 0x2000u) == -410,
                "Create attr / unknown mailbox errors wrong");
        IopCpuState unknown{};
        require(!f.kernel.dispatchMailboxImport(10u, unknown), "undeclared ordinal 10 fabricated success");
    }

    Irx outsideReceiver(bool producer)
    {
        Irx image(0x10000u, 0x600u);
        uint32_t offset = 0u;
        const auto emit = [&](std::initializer_list<uint32_t> words) { image.words(offset, words); offset += 4u * words.size(); };
        const auto pointer = [&](uint32_t reg, uint32_t low) {
            emit({0x3C000001u | (reg << 16u), 0x34000000u | (reg << 21u) | (reg << 16u) | low});
        };
        emit({0x27BDFFE0u, 0xAFBF001Cu, 0xAFB00018u});
        pointer(4u, 0x200u); emit({0x0C004045u, 0u, 0x00408021u}); // CreateMbx @0x10114; s0=id
        pointer(8u, 0x310u); emit({0xAD100000u});
        if (producer)
        {
            pointer(4u, 0x220u); emit({0x0C004065u, 0u, 0x00402021u, 0x00002821u}); // CreateThread @0x10194
            emit({0x0C004067u, 0u}); // StartThread @0x1019c
        }
        pointer(4u, 0x300u); emit({0x02002821u, 0x0C004049u, 0u}); // ReceiveMbx @0x10124, outside kernel thread
        pointer(8u, 0x304u); emit({0xAD020000u, 0x8FB00018u, 0x8FBF001Cu, 0x27BD0020u, 0x00001021u, 0x03E00008u, 0u});
        image.words(0x100u, {0x41E00000u, 0u, 0x0101u, 0x736D6874u, 0x00786267u,
                            0x03E00008u, 0x24000004u, 0x03E00008u, 0x24000006u,
                            0x03E00008u, 0x24000008u, 0u, 0u});
        image.words(0x180u, {0x41E00000u, 0u, 0x0101u, 0x61626874u, 0x00006573u,
                            0x03E00008u, 0x24000004u, 0x03E00008u, 0x24000006u, 0u, 0u});
        image.words(0x200u, {0u, 0x1234u});
        image.words(0x220u, {0u, 0u, 0x10400u, 0x400u, 0x20u});
        image.words(0x240u, {0u, 1u});
        image.words(0x300u, {0xDEADBEEFu, 0xDEADBEEFu, 0u, 0u, 0u});
        offset = 0x400u;
        emit({0x27BDFFF0u, 0xAFBF000Cu});
        pointer(8u, 0x310u); emit({0x8D040000u});
        pointer(5u, 0x240u); emit({0x0C004047u, 0u, 0x8FBF000Cu, 0x27BD0010u, 0x03E00008u, 0u}); // SendMbx @0x1011c
        return image;
    }

    void physicalImportAndOutsideWait()
    {
        for (bool producer : {false, true})
        {
            Host host;
            IopSubsystem iop(host);
            outsideReceiver(producer).install(host);
            const auto result = iop.loadModuleBuffer(0x1000u);
            require(result.handled && result.moduleId > 0 && result.startResult == 0, "physical mailbox IRX failed to load");
            uint32_t output[2]{};
            require(iop.readMemory(0x10300u, output, sizeof(output)), "cannot read physical mailbox results");
            require(static_cast<int32_t>(output[1]) == (producer ? 0 : -417),
                    "outside Receive returned the wrong completion/error code");
            require(output[0] == (producer ? 0x10240u : 0xDEADBEEFu),
                    "outside Receive lost the producer's actual pointer or invented a message");
            for (const auto &line : host.logs)
                require(line.find("missing import thmsgbx") == std::string::npos, "thmsgbx still missing from physical dispatch");
        }
    }
}

int main()
{
    const Test tests[] = {
        {"FIFO, Receive/Poll ABI and no-message errors", fifoAndAbi},
        {"Stable message priority, circular links and status layout", messagePriorityAndStatus},
        {"Receiver FIFO, priority and equal-priority arrival order", receiverOrdering},
        {"Delete, release, suspended waits and reset", deleteReleaseAndSuspend},
        {"Interrupt API context and higher-priority handoff", interruptsAndPreemption},
        {"Physical IRX imports and real outside-thread receive pumping", physicalImportAndOutsideWait},
    };
    return run(tests);
}
