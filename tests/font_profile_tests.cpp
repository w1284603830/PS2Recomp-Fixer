#include "Common.h"
constexpr uint32_t PS2_GS_VRAM_SIZE = 4u * 1024u * 1024u;
template<class T> T fontTestRead(const uint8_t *ram, uint32_t address)
{
    T value; std::memcpy(&value, getConstMemPtr(ram, address), sizeof(value)); return value;
}
template<class T> void fontTestWrite(uint8_t *ram, uint32_t address, T value)
{
    std::memcpy(getMemPtr(ram, address), &value, sizeof(value));
}
#define FAST_READ8(address) fontTestRead<uint8_t>(rdram, address)
#define FAST_READ16(address) fontTestRead<uint16_t>(rdram, address)
#define FAST_WRITE16(address, value) fontTestWrite<uint16_t>(rdram, address, value)
#define FAST_WRITE32(address, value) fontTestWrite<uint32_t>(rdram, address, value)
#define FAST_WRITE64(address, value) fontTestWrite<uint64_t>(rdram, address, value)
#define GPR_U64(ctx, reg) static_cast<uint64_t>(getRegU32(ctx, reg))
#define GPR_S64(ctx, reg) static_cast<int64_t>(static_cast<int32_t>(getRegU32(ctx, reg)))
namespace ps2_stubs
{
    inline void vsprintf(uint8_t *, R5900Context *, PS2Runtime *) {}
    inline uint64_t makeGiftagAplusD(uint32_t) { return 0; }
}
#include "Font.cpp"

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } } while (false)

using namespace ps2_stubs;
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    const std::string mode = argv[1];
    PS2Runtime runtime;
    auto *ram = runtime.memory().getRDRAM();
    R5900Context ctx;
    FontGameProfile p;
    p.table = 0x40000u; p.count = 4u;
    p.signatureAddress = 0x8000u; p.signatureWord = 0x27BDFFA0u;
    p.gpOffsets = {-32, -28, -24, -20, -16, -12, -8, -4};
    fontTestWrite<uint32_t>(ram, p.signatureAddress, p.signatureWord);
    ctx.gpr[28] = 0x20000u;
    if (mode != "unconfigured")
        CHECK(configureFontGameProfile(p));
    if (mode == "invalid")
    {
        auto bad = p; bad.table = 0x1FFFFF0u; CHECK(!configureFontGameProfile(bad));
        bad = p; bad.count = UINT32_MAX; CHECK(!configureFontGameProfile(bad));
        bad = p; bad.gpOffsets[0] = bad.gpOffsets[1]; CHECK(!configureFontGameProfile(bad));
        bad = p; bad.gpOffsets[0] = -31; CHECK(!configureFontGameProfile(bad));
    }
    if (mode == "mismatch")
        fontTestWrite<uint32_t>(ram, p.signatureAddress, 0u);
    if (mode == "unconfigured" || mode == "mismatch" || mode == "invalid")
    {
        const auto before = runtime.mem.ram;
        const PS2Runtime::RecompiledFunction entries[] = {
            sceeFontInit, sceeFontLoadFont, sceeFontGenerateString, sceeFontPrintfAt, sceeFontPrintfAt2,
            sceeFontClose, sceeFontSetColour, sceeFontSetMode, sceeFontSetFont, sceeFontSetScale};
        for (auto entry : entries)
        {
            ctx.gpr[2] = 99u;
            entry(ram, &ctx, &runtime);
            CHECK(ctx.gpr[2] == 0xFFFFFFFFu);
        }
        CHECK(before == runtime.mem.ram && runtime.mem.writes.empty());
        return 0;
    }
    if (mode == "font-id")
    {
        const auto before = runtime.mem.ram;
        for (uint32_t id : {4u, UINT32_MAX})
        {
            ctx.gpr[4] = ctx.gpr[5] = ctx.gpr[11] = id;
            for (auto entry : {sceeFontClose, sceeFontLoadFont, sceeFontGenerateString})
            {
                entry(ram, &ctx, &runtime);
                CHECK(ctx.gpr[2] == 0xFFFFFFFFu && before == runtime.mem.ram);
            }
        }
        return 0;
    }
    if (mode == "source-bounds")
    {
        for (uint32_t source : {PS2_RAM_SIZE - 4u, PS2_RAM_SIZE - 16u, 0x50001u, 0x10002010u})
        {
            ctx.gpr[4] = source; ctx.gpr[5] = 0u;
            const auto before = runtime.mem.ram;
            sceeFontLoadFont(ram, &ctx, &runtime);
            CHECK(ctx.gpr[2] == UINT32_MAX && before == runtime.mem.ram);
        }
        return 0;
    }
    if (mode == "allocation" || mode == "load-close")
    {
        constexpr uint32_t source = 0x50000u;
        ctx.gpr[4] = source; ctx.gpr[5] = 0u; ctx.gpr[7] = 0x100u;
        fontTestWrite<uint32_t>(ram, source, 16u);
        fontTestWrite<uint32_t>(ram, source + 4u, 16u);
        fontTestWrite<uint32_t>(ram, source + 8u, 12u);
        fontTestWrite<uint32_t>(ram, source + 12u, 16u);
        std::fill(ram + source + 16u, ram + source + 16u + 16u + 0x2010u + 0xC400u, 0xA5u);
        if (mode == "allocation")
        {
            for (auto allocations : {std::vector<uint32_t>{0u}, {0x180000u, 0u}, {0x180000u, 0x190000u, 0u}})
            {
                runtime.allocations = allocations; runtime.allocationIndex = 0u; runtime.freed.clear();
                const auto before = runtime.mem.ram;
                sceeFontLoadFont(ram, &ctx, &runtime);
                CHECK(ctx.gpr[2] == UINT32_MAX && before == runtime.mem.ram);
                if (allocations.size() >= 2u)
                    CHECK(std::find(runtime.freed.begin(), runtime.freed.end(), 0x180000u) != runtime.freed.end());
                if (allocations.size() == 3u)
                    CHECK(std::find(runtime.freed.begin(), runtime.freed.end(), 0x190000u) != runtime.freed.end());
            }
            return 0;
        }
        for (bool doubleByte : {false, true})
        {
            fontTestWrite<uint32_t>(ram, source + 8u, doubleByte ? 0x4000000Cu : 12u);
            runtime.allocations = doubleByte ? std::vector<uint32_t>{0x180000u, 0x1B0000u} :
                                               std::vector<uint32_t>{0x180000u, 0x190000u, 0x1B0000u};
            runtime.allocationIndex = 0u; runtime.freed.clear(); ctx.gpr[4] = source;
            sceeFontLoadFont(ram, &ctx, &runtime);
            CHECK(ctx.gpr[2] == 0x100u && readTestWord(ram, p.table) == 0x180000u);
            CHECK(readTestWord(ram, p.table + 0x20u) == (doubleByte ? 1u : 0u));
            CHECK(readTestWord(ram, 0x180000u) == 0xA5A5A5A5u);
            ctx.gpr[4] = 0u;
            sceeFontClose(ram, &ctx, &runtime);
            CHECK(ctx.gpr[2] == 0u && readTestWord(ram, p.table) == 0u);
            CHECK(runtime.freed.size() == (doubleByte ? 1u : 2u));
        }
        return 0;
    }
    if (mode == "close-bounds")
    {
        ctx.gpr[4] = 0u;
        fontTestWrite<uint32_t>(ram, p.table, PS2_RAM_SIZE - 16u);
        auto before = runtime.mem.ram;
        sceeFontClose(ram, &ctx, &runtime);
        CHECK(ctx.gpr[2] == UINT32_MAX && before == runtime.mem.ram && runtime.freed.empty());
        fontTestWrite<uint32_t>(ram, p.table, 0x180000u);
        fontTestWrite<uint32_t>(ram, 0x182000u, 0xFFFFFF00u);
        before = runtime.mem.ram;
        sceeFontClose(ram, &ctx, &runtime);
        CHECK(ctx.gpr[2] == UINT32_MAX && before == runtime.mem.ram && runtime.freed.empty());
        return 0;
    }
    CHECK(mode == "relocated");
    ctx.gpr[4] = 0x12345678u;
    sceeFontSetColour(ram, &ctx, &runtime);
    CHECK(readTestWord(ram, 0x20000u - 16u) == 0x12345678u);
    ctx.gpr[28] = 0u; // Signed GP arithmetic must not wrap into the end of RAM.
    const auto before = runtime.mem.ram;
    sceeFontSetMode(ram, &ctx, &runtime);
    CHECK(ctx.gpr[2] == 0xFFFFFFFFu && before == runtime.mem.ram);
    resetFontGameProfile();
    ctx.gpr[28] = 0x20000u;
    sceeFontSetColour(ram, &ctx, &runtime);
    CHECK(ctx.gpr[2] == 0xFFFFFFFFu && before == runtime.mem.ram);
    return 0;
}
