#pragma once
// Private, independently testable CH4 gather/commit transaction. Caller is the
// EE memory thread; this layer never invokes a decoder, scheduler, or GS callback.
#include "dod_mpeg_dma_ingress.h"
#include <cstring>
#include <limits>
#include <stdexcept>

namespace dod_mpeg_dma
{
    struct Registers
    {
        uint32_t madr = 0, qwc = 0, tadr = 0, asr0 = 0, asr1 = 0;
    };
    template<class Io> Registers readRegisters(const Io &io)
    {
        auto read = [&](uint32_t address)
        { const auto found = io.find(address); return found == io.end() ? 0u : found->second; };
        return {read(0x1000B410u), read(0x1000B420u), read(0x1000B430u),
                read(0x1000B440u), read(0x1000B450u)};
    }
    template<class Io> void commitRegisters(Io &io, const Registers &value)
    {
        io[0x1000B410u] = value.madr; io[0x1000B420u] = value.qwc;
        io[0x1000B430u] = value.tadr; io[0x1000B440u] = value.asr0; io[0x1000B450u] = value.asr1;
    }
    struct Gathered
    {
        Registers final;
        std::vector<uint8_t> bytes;
        uint32_t completedChcr = 0;
    };
    // Same tag traversal and address rules as the original dodGatherToIpu.
    // Only a successful return owns a complete transaction; the source registers
    // are never mutated while gathering or when the bounded active path rejects.
    template<class Resolve, class Advance>
    Gathered gather(Registers initial, uint32_t chcr, Resolve resolve, Advance advance,
                    size_t limit = std::numeric_limits<size_t>::max(), bool trackIpuChainState = false)
    {
        Gathered result; result.final = initial; result.completedChcr = chcr;
        auto append = [&](uint32_t address, uint32_t qwc)
        {
            const size_t count = static_cast<size_t>(qwc) * 16u;
            if (count > limit - result.bytes.size()) throw std::length_error("toIPU transaction exceeds staging bound");
            for (uint32_t i = 0; i < qwc; ++i)
            {
                const uint8_t *p = resolve(address);
                result.bytes.insert(result.bytes.end(), p, p + 16);
                address = advance(address, 16u);
            }
            return address;
        };
        result.final.madr = append(initial.madr, initial.qwc & 0xFFFFu);
        result.final.qwc = 0;
        if (((chcr >> 2) & 3u) != 1u) return result;
        uint32_t tadr = initial.tadr, asr[2] = {initial.asr0, initial.asr1};
        uint32_t asp = (chcr >> 4) & 3u;
        const bool tie = (chcr & 0x80u) != 0u;
        if (trackIpuChainState && (initial.qwc & 0xFFFFu) != 0u)
        {
            // Native stop/restart resumes the already-fetched tag's remainder.
            // Its CHCR.TAG still determines CNT TADR progress and whether another
            // tag may be fetched after this QWC finishes (REFE/END/TIE+IRQ stop).
            const uint32_t previousId = (chcr >> 28) & 7u;
            if (previousId == 1u) result.final.tadr = tadr = result.final.madr;
            if (previousId == 0u || previousId == 7u || (tie && (chcr & 0x80000000u))) return result;
        }
        for (int tags = 0; tags < 65536; ++tags)
        {
            uint64_t tag = 0; std::memcpy(&tag, resolve(tadr), sizeof(tag));
            const uint32_t qwc = static_cast<uint32_t>(tag & 0xFFFFu);
            const uint32_t id = static_cast<uint32_t>((tag >> 28) & 7u);
            const bool irq = ((tag >> 31) & 1u) != 0u;
            const uint32_t addr = static_cast<uint32_t>(tag >> 32);
            bool end = false;
            if (trackIpuChainState)
            {
                // CH4 uses the non-stack source-chain semantics: every tag's
                // upper half is reflected in CHCR, and MADR is the address just
                // past its actual payload. ASP/ASR are not a CH4 call stack.
                result.completedChcr = (chcr & 0xFFFFu) | (static_cast<uint32_t>(tag) & 0xFFFF0000u);
                uint32_t source = addr;
                switch (id)
                {
                case 0: tadr += 16u; end = true; break;
                case 1: source = tadr + 16u; break;
                case 2: source = tadr + 16u; tadr = addr; break;
                case 3: case 4: tadr += 16u; break;
                case 7: source = tadr + 16u; end = true; break;
                default: end = true; break; // Undefined CH4 CALL/RET: no stack traversal.
                }
                result.final.madr = append(source, qwc);
                if (id == 1u) tadr = result.final.madr;
                if (end || (irq && tie)) break;
                continue;
            }
            switch (id)
            {
            case 0: append(addr, qwc); tadr += 16u; end = true; break;
            case 1: tadr = append(tadr + 16u, qwc); break;
            case 2: append(tadr + 16u, qwc); tadr = addr; break;
            case 3: case 4: append(addr, qwc); tadr += 16u; break;
            case 5:
            {
                const uint32_t ret = append(tadr + 16u, qwc);
                if (asp < 2u) asr[asp++] = ret;
                tadr = addr; break;
            }
            case 6:
                append(tadr + 16u, qwc);
                if (asp > 0u) tadr = asr[--asp]; else end = true;
                break;
            default: append(tadr + 16u, qwc); end = true; break;
            }
            if (end || (irq && tie)) break;
        }
        result.final.tadr = tadr; result.final.asr0 = asr[0]; result.final.asr1 = asr[1];
        return result;
    }

    class PendingTransfer
    {
    public:
        bool active() const { return active_; }
        bool oversized() const { return oversized_; }
        Session session() const { return session_; }
        size_t bytes() const { return transfer_.bytes.size(); }
        template<class Resolve, class Advance>
        void stage(Session session, Registers registers, uint32_t chcr, Resolve resolve, Advance advance)
        {
            if (active_) return; // retries cannot recapture or duplicate a packet
            active_ = true; session_ = session;
            try { transfer_ = gather(registers, chcr, resolve, advance, kCapacityBytes, true); }
            catch (const std::length_error &) { oversized_ = true; transfer_ = {}; }
        }
        // Exactly the acceptance point commits MADR/QWC/tag registers. The
        // PS2Memory owner then clears STR and raises the existing DMAC cause.
        template<class Io> SubmitResult service(Ingress &target, Io &io)
        {
            if (!active_) return SubmitResult::Invalid;
            if (oversized_) return SubmitResult::TooLarge;
            const SubmitResult result = target.submit(session_, transfer_.bytes);
            if (result == SubmitResult::Accepted)
            { commitRegisters(io, transfer_.final); io[0x1000B400u] = transfer_.completedChcr; abort(); }
            return result;
        }
        // Hardware stop/reset only: no register progress, completion, or EOS.
        void abort() { active_ = false; oversized_ = false; session_ = {}; transfer_ = {}; }
    private:
        bool active_ = false, oversized_ = false;
        Session session_;
        Gathered transfer_;
    };
}
