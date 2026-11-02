#include "IPU.cpp" // Python places the unmodified production file in the temporary include directory.
#include <algorithm>
#include <string>

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } } while (false)

int main(int argc, char **argv)
{
    CHECK(argc >= 2);
    const std::string mode = argv[1];
    PS2Runtime runtime;
    R5900Context ctx;
    uint8_t *ram = runtime.memory().getRDRAM();
    if (mode == "unconfigured")
    {
        ps2_stubs::sceIpuInit(ram, &ctx, &runtime);
        CHECK(ctx.result == -1 && runtime.loads.empty() && runtime.mem.writes32.empty());
        CHECK(runtime.scheduler.binds == 0);
        return 0;
    }
    if (mode == "reset")
    {
        ps2_stubs::dodConfigureIpuInit(0x1000u, 0x2000u);
        CHECK(ps2_stubs::hasValidIpuInitConfig());
        ps2_stubs::resetIpuInitConfig();
        CHECK(!ps2_stubs::hasValidIpuInitConfig());
        ps2_stubs::sceIpuInit(ram, &ctx, &runtime);
        CHECK(ctx.result == -1 && runtime.loads.empty() && runtime.mem.writes32.empty());
        return 0;
    }
    uint32_t iq = 0x1000u, vq = 0x2000u, helper = 0u;
    if (mode == "invalid-segment") iq = 0xC0001000u;
    if (mode == "invalid-iq") iq = 0x1ffffc0u; // 64 bytes remain, but the IQ source needs 80.
    if (mode == "invalid-vq") vq = 0x1fffff0u;
    if (mode == "unaligned-iq") iq += 1;
    if (mode == "unaligned-vq") vq += 1;
    if (mode == "invalid-helper") helper = 0x2000000u;
    if (mode == "unaligned-helper") helper = 0x3001u;
    if (mode == "last-byte") { iq = 0x1ffffb0u; vq = 0x1ffffe0u; }
    if (mode == "mirrors") { iq |= 0x80000000u; vq |= 0x20000000u; }
    if (mode == "helper" || mode == "missing-helper") helper = 0x3000u;
    if (mode == "helper") runtime.registeredHelper = helper;
    if (mode == "memory-failure") { runtime.mem.ready = false; runtime.mem.canInitialize = false; }
    if (mode == "sync-failure") runtime.canSync = false;
    ps2_stubs::dodConfigureIpuInit(iq, vq, helper);
    if (mode == "null-runtime")
    {
        ps2_stubs::sceIpuInit(ram, &ctx, nullptr);
        CHECK(ctx.result == -1);
        return 0;
    }
    bool invoked = false;
    try { ps2_stubs::sceIpuInit(ram, &ctx, &runtime); }
    catch (const InvocationRequested &)
    {
        invoked = true;
        CHECK(mode == "helper");
        CHECK(runtime.loads.empty() && runtime.mem.writes32.empty());
        CHECK(runtime.scheduler.pending.context.pc == helper);
        CHECK(runtime.scheduler.pending.context.gpr[4] == 1);
        runtime.scheduler.pending.onComplete(runtime.scheduler.pending.context, ctx);
    }
    if (mode.rfind("invalid-", 0) == 0 || mode.rfind("unaligned-", 0) == 0 ||
        mode == "memory-failure" || mode == "sync-failure")
    {
        CHECK(ctx.result == -1 && runtime.loads.empty() && runtime.mem.writes32.empty());
        CHECK(!invoked);
        return 0;
    }
    CHECK(ctx.result == 0);
    CHECK(runtime.loads == std::vector<uint32_t>({iq, iq + 16, iq + 32, iq + 48, iq + 64, vq, vq + 16}));
    CHECK(runtime.mem.writes128.size() == 10);
    CHECK(runtime.mem.writes128[4].second == runtime.mem.writes128[5].second);
    CHECK(runtime.mem.writes128[4].second == runtime.mem.writes128[7].second);
    const auto direct = std::count(runtime.mem.writes32.begin(), runtime.mem.writes32.end(),
                                   std::make_pair(0x1000B400u, 1u));
    CHECK(direct == (mode == "helper" ? 0 : 1));
    CHECK(invoked == (mode == "helper"));
    return 0;
}
