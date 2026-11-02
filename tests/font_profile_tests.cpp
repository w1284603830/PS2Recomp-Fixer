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
