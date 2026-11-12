// Production ps2_overlays.cpp on its own (no runtime, no game data): registration, signature dispatch, clear,
// copying and concurrent lookups. Synthetic addresses only. Usage: overlay_registry <case>
#include "ps2_overlays.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace
{
    int g_failures = 0;
#define CHECK(condition)                                                                              \
    do                                                                                                \
    {                                                                                                 \
        if (!(condition))                                                                             \
        {                                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                 \
            ++g_failures;                                                                             \
        }                                                                                             \
    } while (false)

    constexpr uint32_t kBase = 0x00600000u;
    void fnA(uint8_t *, R5900Context *, PS2Runtime *) {}
    void fnB(uint8_t *, R5900Context *, PS2Runtime *) {}
    void fnOuter(uint8_t *, R5900Context *, PS2Runtime *) {}
    const uint8_t kSigA[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x01};
    const uint8_t kSigB[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x02};
    const ps2_overlays::Entry kEntriesA[] = {{kBase + 0x100u, &fnA}, {kBase + 0x200u, &fnA}};
    const ps2_overlays::Entry kEntriesB[] = {{kBase + 0x100u, &fnB}};

    ps2_overlays::Image image(const char *name, uint32_t load, uint32_t size, const uint8_t *sig, uint32_t sigSize,
                              const ps2_overlays::Entry *entries, uint32_t count, uint32_t sigOffset = 0u)
    {
        ps2_overlays::Image result;
        result.name = name;
        result.loadAddress = load;
        result.imageSize = size;
        result.signatureOffset = sigOffset;
        result.signature = sig;
        result.signatureSize = sigSize;
        result.entries = entries;
        result.entryCount = count;
        return result;
    }

    using ps2_overlays::Resolution;
    using ps2_overlays::Status;

    void caseEmpty()
    {
        ps2_overlays::clear();
        std::vector<uint8_t> ram(ps2_overlays::kGuestRamSize, 0u);
        for (uint32_t address : {0u, 4u, kBase, kBase + 0x100u, ps2_overlays::kGuestRamSize - 4u, 0xFFFFFFFCu})
        {
            CHECK(!ps2_overlays::mayCover(address));
            CHECK(ps2_overlays::resolve(ram.data(), address).resolution == Resolution::NotCovered);
        }
        CHECK(ps2_overlays::registeredCount() == 0u);
        CHECK(ps2_overlays::describeResident(ram.data()).empty());
    }

    void caseDispatch()
    {
        ps2_overlays::clear();
        std::vector<uint8_t> ram(ps2_overlays::kGuestRamSize, 0u);
        CHECK(ps2_overlays::registerImage(image("a", kBase, 0x1000u, kSigA, 8u, kEntriesA, 2u)) == Status::Ok);
        CHECK(ps2_overlays::registerImage(image("b", kBase, 0x400u, kSigB, 8u, kEntriesB, 1u)) == Status::Ok);
        CHECK(ps2_overlays::resolve(ram.data(), kBase + 0x100u).resolution == Resolution::NotCovered);
        std::memcpy(ram.data() + kBase, kSigA, 8u);
        CHECK(ps2_overlays::resolve(ram.data(), kBase + 0x100u).function == &fnA);
        CHECK(ps2_overlays::resolve(ram.data(), kBase + 0x200u).function == &fnA);
        std::memcpy(ram.data() + kBase, kSigB, 8u);
        CHECK(ps2_overlays::resolve(ram.data(), kBase + 0x100u).function == &fnB);
        CHECK(ps2_overlays::resolve(ram.data(), kBase + 0x200u).resolution == Resolution::Missing);
        CHECK(ps2_overlays::resolve(ram.data(), kBase + 0x400u).resolution == Resolution::NotCovered);
        CHECK(ps2_overlays::resolve(nullptr, kBase + 0x100u).resolution == Resolution::NotCovered);
        CHECK(std::string(ps2_overlays::residentImage(ram.data(), kBase)) == "b");
        CHECK(ps2_overlays::residentImage(ram.data(), kBase + 4u) == nullptr);
        ram[kBase + 7u] = 0x03; // neither signature
        CHECK(ps2_overlays::resolve(ram.data(), kBase + 0x100u).resolution == Resolution::NotCovered);
        CHECK(ps2_overlays::residentImage(ram.data(), kBase) == nullptr);

        // A larger image at a lower address keeps the parts the smaller one does not cover.
        const uint32_t outerBase = kBase - 0x10000u;
        const ps2_overlays::Entry outer[] = {{kBase + 0x100u, &fnOuter}, {kBase + 0x2000u, &fnOuter}};
        const uint8_t outerSig[] = {0xAB, 0xCD};
        CHECK(ps2_overlays::registerImage(image("outer", outerBase, 0x20000u, outerSig, 2u, outer, 2u)) == Status::Ok);
        std::memcpy(ram.data() + outerBase, outerSig, 2u);
        CHECK(ps2_overlays::resolve(ram.data(), kBase + 0x100u).function == &fnOuter);
        std::memcpy(ram.data() + kBase, kSigA, 8u);
        CHECK(ps2_overlays::resolve(ram.data(), kBase + 0x100u).function == &fnA);
        CHECK(ps2_overlays::resolve(ram.data(), kBase + 0x2000u).function == &fnOuter);
        CHECK(ps2_overlays::describeResident(ram.data()) == "0x00600000=a 0x005F0000=outer");
        ps2_overlays::clear();
    }

    void caseReject()
    {
        ps2_overlays::clear();
        const ps2_overlays::Entry unsorted[] = {{kBase + 0x200u, &fnA}, {kBase + 0x100u, &fnA}};
        const ps2_overlays::Entry outside[] = {{kBase + 0x1000u, &fnA}};
        const uint8_t big[257] = {};
        CHECK(ps2_overlays::registerImage(image("x", kBase, 0x1000u, kSigA, 0u, kEntriesA, 2u)) == Status::InvalidImage);
        CHECK(ps2_overlays::registerImage(image("x", kBase, 0x1000u, big, 257u, kEntriesA, 2u)) == Status::InvalidImage);
        CHECK(ps2_overlays::registerImage(image("x", kBase, 0x1000u, kSigA, 8u, kEntriesA, 2u, 0xFF9u)) == Status::InvalidImage);
        CHECK(ps2_overlays::registerImage(image("x", 0xFFFFF000u, 0x2000u, kSigA, 8u, kEntriesA, 2u)) == Status::InvalidImage);
        CHECK(ps2_overlays::registerImage(image("x", kBase, 0x1000u, kSigA, 8u, unsorted, 2u)) == Status::BadEntries);
        CHECK(ps2_overlays::registerImage(image("x", kBase, 0x1000u, kSigA, 8u, outside, 1u)) == Status::BadEntries);
        CHECK(ps2_overlays::registerImage(image("a", kBase, 0x1000u, kSigA, 8u, kEntriesA, 2u)) == Status::Ok);
        CHECK(ps2_overlays::registerImage(image("a", kBase + 0x4000u, 0x1000u, kSigB, 8u, kEntriesB, 0u)) == Status::InvalidImage);
        const ps2_overlays::Entry elsewhere[] = {{kBase + 0x4100u, &fnB}};
        CHECK(ps2_overlays::registerImage(image("a", kBase + 0x4000u, 0x1000u, kSigB, 8u, elsewhere, 1u)) == Status::DuplicateName);
        CHECK(ps2_overlays::registerImage(image("prefix", kBase, 0x1000u, kSigA, 4u, kEntriesB, 1u)) == Status::AmbiguousSignature);
        CHECK(ps2_overlays::registerImage(image("apart", kBase, 0x1000u, kSigB, 8u, kEntriesB, 1u, 0x10u)) == Status::AmbiguousSignature);
        CHECK(ps2_overlays::registerImage(image("b", kBase, 0x1000u, kSigB, 8u, kEntriesB, 1u)) == Status::Ok);
        CHECK(ps2_overlays::registeredCount() == 2u);
        CHECK(std::string(ps2_overlays::describe(Status::AmbiguousSignature)).find("signature") != std::string::npos);
        ps2_overlays::clear();
    }

    void caseClear()
    {
        ps2_overlays::clear();
        std::vector<uint8_t> ram(ps2_overlays::kGuestRamSize, 0u);
        std::memcpy(ram.data() + kBase, kSigA, 8u);
        CHECK(ps2_overlays::registerImage(image("a", kBase, 0x1000u, kSigA, 8u, kEntriesA, 2u)) == Status::Ok);
        CHECK(ps2_overlays::mayCover(kBase + 0x100u));
        ps2_overlays::clear();
        CHECK(!ps2_overlays::mayCover(kBase + 0x100u));
        CHECK(ps2_overlays::registeredCount() == 0u);
        CHECK(ps2_overlays::resolve(ram.data(), kBase + 0x100u).resolution == Resolution::NotCovered);
        CHECK(ps2_overlays::registerImage(image("a", kBase, 0x1000u, kSigA, 8u, kEntriesA, 2u)) == Status::Ok);
        CHECK(ps2_overlays::resolve(ram.data(), kBase + 0x100u).function == &fnA);
        ps2_overlays::clear();
    }

    void caseCopy()
    {
        ps2_overlays::clear();
        std::vector<uint8_t> ram(ps2_overlays::kGuestRamSize, 0u);
        {
            std::vector<char> name{'t', 'm', 'p', '\0'};
            std::vector<uint8_t> sig(kSigA, kSigA + 8);
            std::vector<ps2_overlays::Entry> entries{{kBase + 0x100u, &fnB}};
            CHECK(ps2_overlays::registerImage(image(name.data(), kBase, 0x1000u, sig.data(), 8u, entries.data(), 1u)) == Status::Ok);
        } // freed: AddressSanitizer reports any use of the caller's storage
        std::memcpy(ram.data() + kBase, kSigA, 8u);
        const ps2_overlays::Lookup lookup = ps2_overlays::resolve(ram.data(), kBase + 0x100u);
        CHECK(lookup.function == &fnB);
        CHECK(lookup.image != nullptr && std::string(lookup.image) == "tmp");
        ps2_overlays::clear();
    }

    // Threads look up with different RAM contents at once, so the per-area resident cache keeps changing hands.
    // Every lookup must still match the signature of the RAM it was given.
    void caseThreads()
    {
        ps2_overlays::clear();
        std::vector<uint8_t> ramA(ps2_overlays::kGuestRamSize, 0u);
        std::vector<uint8_t> ramB(ps2_overlays::kGuestRamSize, 0u);
        std::memcpy(ramA.data() + kBase, kSigA, 8u);
        std::memcpy(ramB.data() + kBase, kSigB, 8u);
        CHECK(ps2_overlays::registerImage(image("a", kBase, 0x1000u, kSigA, 8u, kEntriesA, 2u)) == Status::Ok);
        CHECK(ps2_overlays::registerImage(image("b", kBase, 0x1000u, kSigB, 8u, kEntriesB, 1u)) == Status::Ok);
        std::atomic<int> wrong{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t)
        {
            threads.emplace_back([&, t]()
                                 {
                for (int i = 0; i < 20000; ++i)
                {
                    const bool useA = ((i + t) & 1) == 0;
                    const ps2_overlays::Lookup lookup =
                        ps2_overlays::resolve(useA ? ramA.data() : ramB.data(), kBase + 0x100u);
                    if (lookup.function != (useA ? &fnA : &fnB))
                        wrong.fetch_add(1, std::memory_order_relaxed);
                } });
        }
        for (std::thread &thread : threads)
            thread.join();
        CHECK(wrong.load() == 0);
        ps2_overlays::clear();
    }
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    const std::string mode = argv[1];
    if (mode == "empty")
        caseEmpty();
    else if (mode == "dispatch")
        caseDispatch();
    else if (mode == "reject")
        caseReject();
    else if (mode == "clear")
        caseClear();
    else if (mode == "copy")
        caseCopy();
    else if (mode == "threads")
        caseThreads();
    else
        return 2;
    return g_failures == 0 ? 0 : 1;
}
