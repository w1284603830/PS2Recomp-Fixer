#include "iop_compat_test_support.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace
{
    using namespace iop_test;
    constexpr uint32_t kBase = 0x10000u;
    constexpr uint32_t kCallback = kBase + 0x200u;
    constexpr uint32_t kImport = kBase + 0x414u;
    constexpr uint32_t kSource = kBase + 0x500u;
    constexpr uint32_t kDescriptor = kBase + 0x520u;
    constexpr uint32_t kResults = kBase + 0x600u;
    constexpr uint32_t kCallbackResults = kBase + 0x680u;
    constexpr uint32_t kDestination = 0x18000u;
    constexpr uint32_t kUserdata = 0xA55A11FFu;
    constexpr uint32_t kGp = 0x12345678u;
    constexpr uint32_t kPoison = 0xBAD00BADu;
    constexpr std::array<uint32_t, 4> kPayload{0x53494621u, 0x01234567u, 0x89ABCDEFu, 0xAA55AA55u};

    uint32_t immediate(uint32_t op, uint32_t rs, uint32_t rt, uint16_t value)
    {
        return (op << 26u) | (rs << 21u) | (rt << 16u) | value;
    }

    class Code
    {
    public:
        Code(Irx &image, uint32_t offset) : image(image), cursor(offset) {}
        void emit(uint32_t instruction) { image.words(cursor, {instruction}); cursor += 4u; }
        void li(uint32_t rt, uint32_t value)
        {
            emit(immediate(15u, 0u, rt, static_cast<uint16_t>(value >> 16u)));
            emit(immediate(13u, rt, rt, static_cast<uint16_t>(value)));
        }
        void sw(uint32_t rt, uint32_t offset, uint32_t base = 25u)
        {
            emit(immediate(43u, base, rt, static_cast<uint16_t>(offset)));
        }
        void lw(uint32_t rt, uint32_t offset, uint32_t base = 25u)
        {
            emit(immediate(35u, base, rt, static_cast<uint16_t>(offset)));
            emit(0u); // also safe on an IOP implementation with a load delay
        }
        Irx &image;
        uint32_t cursor;
    };

    struct Scenario
    {
        uint16_t ordinal = 32u;
        uint32_t descriptor = kDescriptor;
        uint32_t count = 1u;
        uint32_t source = kSource;
        uint32_t destination = kDestination;
        uint32_t callback = kCallback;
        bool invalidSecond = false;
    };

    Irx makeImage(const Scenario &scenario)
    {
        Irx image(kBase, 0x800u);
        Code entry(image, 0u);
        entry.emit(0x27BDFFE0u); // addiu sp,sp,-32
        entry.sw(31u, 28u, 29u);
        entry.li(9u, 0xCA11CA11u);
        entry.sw(9u, 16u, 29u);
        entry.li(9u, 0xAABBCCDDu);
        entry.emit((9u << 21u) | 0x11u); // mthi t1
        entry.li(9u, 0xDDEEFF00u);
        entry.emit((9u << 21u) | 0x13u); // mtlo t1
        entry.li(3u, 0xDEADBEEFu);
        entry.li(8u, 0x55AA33CCu);
        entry.li(16u, 0x13572468u);
        entry.li(17u, 0x24681357u);
        entry.li(28u, kGp);
        entry.li(4u, scenario.descriptor);
        entry.li(5u, scenario.count);
        entry.li(6u, scenario.callback);
        entry.li(7u, kUserdata);
        entry.emit(0x0C000000u | (kImport >> 2u));
        entry.emit(0u);
        entry.li(25u, kResults);
        const uint32_t registers[] = {2u, 3u, 8u, 16u, 17u, 28u, 29u, 4u, 5u, 6u, 7u};
        for (uint32_t i = 0; i < std::size(registers); ++i) entry.sw(registers[i], i * 4u);
        entry.emit((10u << 11u) | 0x10u); // mfhi t2
        entry.sw(10u, 44u);
        entry.emit((10u << 11u) | 0x12u); // mflo t2
        entry.sw(10u, 48u);
        entry.lw(10u, 16u, 29u);
        entry.sw(10u, 52u);
        entry.lw(31u, 28u, 29u);
        entry.li(2u, 0x77u); // reaching this result proves caller PC/RA continuation
        entry.emit(0x03E00008u);
        entry.emit(0x27BD0020u);
        require(entry.cursor < 0x200u, "entry overlapped completion function");

        Code callback(image, 0x200u);
        callback.li(25u, kCallbackResults);
        callback.sw(4u, 0u);
        callback.sw(28u, 4u);
        callback.sw(29u, 8u);
        callback.lw(10u, 12u);
        callback.emit(immediate(9u, 10u, 10u, 1u));
        callback.sw(10u, 12u);
        // If completion runs before the copy, the EE receives this poison.
        callback.li(10u, kPoison);
        callback.li(9u, kSource);
        callback.sw(10u, 0u, 9u);
        callback.sw(10u, 16u, 29u); // caller stack cookie must remain intact
        callback.emit((10u << 21u) | 0x11u);
        callback.emit((10u << 21u) | 0x13u);
        for (uint32_t reg : {3u, 4u, 5u, 6u, 7u, 8u, 16u, 17u, 28u}) callback.li(reg, kPoison);
        callback.li(2u, 0xFFFFFFFFu); // must not replace the caller's DMA id
        callback.emit(0x03E00008u);
        callback.emit(0u);
        require(callback.cursor < 0x400u, "completion overlapped import table");

        image.words(0x400u, {0x41E00000u, 0u, 0x0102u, 0x6D666973u, 0x00006E61u,
                            0x03E00008u, 0x24000000u | scenario.ordinal, 0u, 0u});
        image.words(0x500u, {kPayload[0], kPayload[1], kPayload[2], kPayload[3]});
        image.words(0x520u, {scenario.source, scenario.destination, sizeof(kPayload), 0u});
        image.words(0x530u, {scenario.invalidSecond ? 0x01000000u : kSource,
                            kDestination + 32u, sizeof(kPayload), 0u});
        return image;
    }

    uint32_t word(const IopSubsystem &iop, uint32_t address)
    {
        uint32_t value = 0;
        require(iop.readMemory(address, &value, sizeof(value)), "cannot read IOP test result");
        return value;
    }

    void check(const Scenario &scenario, bool transferSucceeds, bool callbackRuns)
    {
        Host host;
        IopSubsystem iop(host);
        auto image = makeImage(scenario);
        image.install(host);
        const auto loaded = iop.loadModuleBuffer(0x1000u);
        require(loaded.handled && loaded.startResult == 0x77, "caller did not continue and return after SIF import");
        const uint32_t dmaId = word(iop, kResults);
        require(transferSucceeds ? dmaId > 0u && dmaId < 0x80000000u : dmaId == 0u,
                "SIF import returned an incorrect success/failure DMA id");
        require(word(iop, kResults + 4u) == 0xDEADBEEFu, "completion clobbered caller v1");
        require(word(iop, kResults + 8u) == 0x55AA33CCu, "completion clobbered caller t0");
        require(word(iop, kResults + 12u) == 0x13572468u, "completion clobbered caller s0");
        require(word(iop, kResults + 16u) == 0x24681357u, "completion clobbered caller s1");
        require(word(iop, kResults + 20u) == kGp, "completion clobbered caller gp");
        require(word(iop, kResults + 28u) == scenario.descriptor, "completion clobbered caller a0");
        require(word(iop, kResults + 32u) == scenario.count, "completion clobbered caller a1");
        require(word(iop, kResults + 36u) == scenario.callback, "completion clobbered caller a2");
        require(word(iop, kResults + 40u) == kUserdata, "completion clobbered caller a3");
        require(word(iop, kResults + 44u) == 0xAABBCCDDu, "completion clobbered caller HI");
        require(word(iop, kResults + 48u) == 0xDDEEFF00u, "completion clobbered caller LO");
        require(word(iop, kResults + 52u) == 0xCA11CA11u, "completion overwrote caller's live stack");
        require(word(iop, kCallbackResults + 12u) == (callbackRuns ? 1u : 0u), "unexpected completion count");
        if (callbackRuns)
        {
            require(word(iop, kCallbackResults) == kUserdata, "completion a0 is not original a3 userdata");
            require(word(iop, kCallbackResults + 4u) == kGp, "completion did not inherit caller gp");
            require(word(iop, kCallbackResults + 8u) != word(iop, kResults + 24u), "completion reused caller's active stack");
            require(word(iop, kSource) == kPoison, "completion body did not execute");
        }
        else require(word(iop, kSource) == kPayload[0], "unexpected callback mutated source");
        if (transferSucceeds)
            require(std::memcmp(host.guest.data() + kDestination, kPayload.data(), sizeof(kPayload)) == 0,
                    "DMA payload incorrect or completion ran before copy");
        else
            require(std::all_of(host.guest.begin() + kDestination, host.guest.begin() + kDestination + 48u,
                                [](uint8_t b) { return b == 0xCCu; }), "failed descriptor chain partially wrote EE RAM");
        iop.runEeCycles(8u * 4096u);
        require(word(iop, kCallbackResults + 12u) == (callbackRuns ? 1u : 0u), "completion repeated or failure queued a delayed callback");
    }

    void success() { check({}, true, true); }
    void noCallback() { Scenario s; s.callback = 0u; check(s, true, false); }
    void plainDma() { Scenario s; s.ordinal = 7u; check(s, true, false); }
    void zeroCount() { Scenario s; s.count = 0u; check(s, false, false); }
    void excessiveCount() { Scenario s; s.count = 33u; check(s, false, false); }
    void nullDescriptor() { Scenario s; s.descriptor = 0u; check(s, false, false); }
    void invalidDescriptor() { Scenario s; s.descriptor = 0x01000000u; check(s, false, false); }
    void invalidSource() { Scenario s; s.source = 0x01000000u; check(s, false, false); }
    void invalidDestination() { Scenario s; s.destination = 0x01000000u; check(s, false, false); }
    void invalidChain() { Scenario s; s.count = 2u; s.invalidSecond = true; check(s, false, false); }
}

int main()
{
    const iop_test::Test tests[] = {
        {"sifman32 transfer, completion arguments, ordering, and caller context", success},
        {"sifman32 null completion still transfers", noCallback},
        {"sifman7 ignores completion registers", plainDma},
        {"sifman32 zero count rejects without completion", zeroCount},
        {"sifman32 count over limit rejects without completion", excessiveCount},
        {"sifman32 null descriptor rejects without completion", nullDescriptor},
        {"sifman32 invalid descriptor rejects without completion", invalidDescriptor},
        {"sifman32 invalid source rejects without completion", invalidSource},
        {"sifman32 invalid EE destination rejects without completion", invalidDestination},
        {"sifman32 invalid second descriptor prevents partial copy and completion", invalidChain},
    };
    return iop_test::run(tests);
}
