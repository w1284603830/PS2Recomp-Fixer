#include "MPEG.cpp" // Full production source, copied unmodified by the Python harness.

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } } while (false)

using namespace ps2_stubs;

static MpegGameProfile profile(MpegAbi abi)
{
    MpegGameProfile p;
    p.abi = abi;
    p.guards[0] = {0x8000u, 0x27BDFFA0u};
    p.defaultCallbacks = {0x21000u, 0x22000u};
    p.objectGlobal = 0x61000u;
    p.referenceTable = 0x62000u;
    p.resetGlobal = 0x63000u;
    for (size_t i = 0; i < p.references.size(); ++i)
        p.references[i] = 0x70000u + static_cast<uint32_t>(i) * 0x80u;
    return p;
}

static void original(uint8_t *, R5900Context *ctx, PS2Runtime *) { setReturnU32(ctx, 42); }

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    const std::string mode = argv[1];
    PS2Runtime runtime;
    auto *ram = runtime.memory().getRDRAM();
    R5900Context ctx;
    auto p = profile(mode == "legacy" ? MpegAbi::Legacy118 : MpegAbi::Work19A0);
    mpegWrite32(ram, p.guards[0].address, p.guards[0].instruction);
    if (mode != "unconfigured")
        CHECK(configureMpegGameProfile(p));

    if (mode == "unconfigured" || mode == "mismatch")
    {
        if (mode == "mismatch")
            mpegWrite32(ram, p.guards[0].address, 0u);
        const auto before = runtime.mem.ram;
        const PS2Runtime::RecompiledFunction entries[] = {
            sceMpegFlush, sceMpegAddBs, sceMpegAddCallback, sceMpegAddStrCallback, sceMpegClearRefBuff,
            sceMpegCreate, sceMpegDelete, sceMpegDemuxPss, sceMpegDemuxPssRing, sceMpegDispCenterOffX,
            sceMpegDispCenterOffY, sceMpegDispHeight, sceMpegDispWidth, sceMpegGetDecodeMode,
            sceMpegGetPicture, sceMpegGetPictureRAW8, sceMpegGetPictureRAW8xy, sceMpegInit,
            sceMpegIsEnd, sceMpegIsRefBuffEmpty, sceMpegReset, sceMpegResetDefaultPtsGap,
            sceMpegSetDecodeMode, sceMpegSetDefaultPtsGap, sceMpegSetImageBuff};
        for (auto entry : entries)
        {
            ctx.gpr[2] = 99u;
            entry(ram, &ctx, &runtime);
            CHECK(ctx.gpr[2] == 0u || ctx.gpr[2] == 0xFFFFFFFFu);
        }
        CHECK(runtime.mem.ram == before && runtime.mem.writes.empty() && runtime.scheduler.calls == 0);
        CHECK(g_mpeg_stub_state.playbackByMpeg.empty());
        return 0;
    }
    if (mode == "invalid")
    {
        std::vector<MpegGameProfile> invalid;
        auto bad = p; bad.abi = MpegAbi::Unsupported; invalid.push_back(bad);
        bad = p; bad.abi = static_cast<MpegAbi>(99); invalid.push_back(bad);
        bad = p; bad.guards = {}; invalid.push_back(bad);
        bad = p; bad.guards[0].address = 0x8001u; invalid.push_back(bad);
        bad = p; bad.guards[0].address = 0x10002010u; invalid.push_back(bad);
        bad = p; bad.defaultCallbacks[0] = 0u; invalid.push_back(bad);
        bad = p; bad.defaultCallbacks[1] = 0x2000000u; invalid.push_back(bad);
        bad = p; bad.defaultCallbacks[1] = 0xC0022000u; invalid.push_back(bad);
        bad = profile(MpegAbi::Legacy118); bad.referenceTable = 0x1FFFFE0u; invalid.push_back(bad);
        bad = profile(MpegAbi::Legacy118); bad.references[8] = 0x1FFFFA0u; invalid.push_back(bad);
        bad = p; bad.movieCount = UINT32_MAX; bad.movieFlagsStride = UINT32_MAX - 1u;
        bad.movieFlags = 0x9000u; invalid.push_back(bad);
        bad = p; bad.hdEntries[0] = 0x3000u; invalid.push_back(bad);
        for (const auto &candidate : invalid)
        {
            CHECK(!configureMpegGameProfile(candidate));
            const auto before = runtime.mem.ram;
            sceMpegCreate(ram, &ctx, &runtime);
            CHECK(ctx.gpr[2] == 0u && before == runtime.mem.ram);
        }
        return 0;
    }
    if (mode == "field")
    {
        ctx.pc = 0x24000u;
        CHECK(!mpegMovieFieldRead(ram, &ctx));
        p.movieFieldReadPc = ctx.pc; p.movieFieldReadInstruction = 0x8C281000u;
        CHECK(configureMpegGameProfile(p));
        CHECK(!mpegMovieFieldRead(ram, &ctx));
        mpegWrite32(ram, ctx.pc, p.movieFieldReadInstruction);
        CHECK(mpegMovieFieldRead(ram, &ctx));
        resetMpegGameProfile(&runtime);
        CHECK(!mpegMovieFieldRead(ram, &ctx));
        return 0;
    }
    if (mode == "movie")
    {
        p.movieIdGpOffset = -0x100;
        p.movieCount = 3;
        p.movieFlags = 0x9000u;
        p.movieFlagsStride = 8;
        CHECK(configureMpegGameProfile(p));
        ctx.gpr[28] = 0x4000u;
        mpegWrite32(ram, 0x3F00u, 2u);
        const uint16_t expected = 0x1234u;
        std::memcpy(ram + 0x9010u, &expected, 2);
        uint32_t movie; uint16_t flags;
        CHECK(readProfileMovie(ram, &ctx, movie, flags) && movie == 2 && flags == expected);
        mpegWrite32(ram, 0x3F00u, 3u);
        CHECK(!readProfileMovie(ram, &ctx, movie, flags));
        ctx.gpr[28] = 0u;
        CHECK(!readProfileMovie(ram, &ctx, movie, flags));
        ctx.gpr[28] = 0x10002110u;
        CHECK(!readProfileMovie(ram, &ctx, movie, flags));
        return 0;
    }
    if (mode == "hooks" || mode == "incomplete-hooks")
    {
        p.movieCount = 3; p.movieFlags = 0x9000u; p.movieFlagsStride = 8;
        for (size_t i = 0; i < p.hdEntries.size(); ++i)
        {
            p.hdEntries[i] = 0x50000u + static_cast<uint32_t>(i) * 4u;
            runtime.functions[p.hdEntries[i]] = original;
        }
        auto bad = p; bad.hdEntries[1] = bad.hdEntries[0];
        CHECK(!validMpegGameProfile(bad));
        CHECK(configureMpegGameProfile(p));
        if (mode == "incomplete-hooks")
            runtime.functions.erase(p.hdEntries.back());
        const auto before = runtime.functions;
        dodInstallHdMovieHooks(runtime);
        if (mode == "incomplete-hooks")
        {
            CHECK(before == runtime.functions && !hdHookRuntime);
            return 0;
        }
        CHECK(runtime.functions != before);
        const auto installed = runtime.functions;
        dodInstallHdMovieHooks(runtime);
        CHECK(runtime.functions == installed);
        ctx.pc = p.hdEntries[0];
        runtime.lookupFunction(ctx.pc)(ram, &ctx, &runtime);
        CHECK(ctx.gpr[2] == 42u); // No HD host support: preserve the native path.
        CHECK(!configureMpegGameProfile(p)); // Live hook replacement requires an explicit reset.
        resetMpegGameProfile(&runtime);
        CHECK(runtime.functions == before && !g_mpeg_profile_valid && !dod_mpeg_dma::activeSession());
        return 0;
    }
    ctx.gpr[4] = 0x20000u;
    ctx.gpr[5] = 0x30000u;
    ctx.gpr[6] = 0x4000u;
    mpegWrite32(ram, 0x1717BCu, 0xABCDu); // The old global must never be touched.
    sceMpegCreate(ram, &ctx, &runtime);
    CHECK(mpegRead32(ram, 0x1717BCu) == 0xABCDu);
    if (mode == "work19a0")
    {
        CHECK(ctx.gpr[2] == 1u);
        CHECK(mpegRead32(ram, 0x30024u) == p.defaultCallbacks[0]);
        CHECK(mpegRead32(ram, 0x30030u) == p.defaultCallbacks[1]);
        CHECK(dod_mpeg_dma::activeSession().mpegAddr == 0x20000u);
    }
    else
    {
        CHECK(mode == "legacy" && ctx.gpr[2] != 0);
        CHECK(mpegRead32(ram, p.objectGlobal) == 0x20000u);
        CHECK(mpegRead32(ram, 0x3001Cu) == p.defaultCallbacks[0]);
        CHECK(mpegRead32(ram, 0x30024u) == p.defaultCallbacks[1]);
        for (size_t i = 0; i < p.references.size(); ++i)
        {
            CHECK(mpegRead32(ram, p.referenceTable + static_cast<uint32_t>(i) * 4u) == p.references[i]);
            mpegWrite32(ram, p.references[i] + 0x28u, 123u);
        }
        sceMpegClearRefBuff(ram, &ctx, &runtime);
        for (size_t i = 0; i < p.references.size(); ++i)
            CHECK(mpegRead32(ram, p.references[i] + 0x28u) == (i % 3u == 2u ? 123u : 0u));
        mpegWrite32(ram, p.resetGlobal, 99u);
        sceMpegReset(ram, &ctx, &runtime);
        CHECK(mpegRead32(ram, p.resetGlobal) == 0u);
    }
    resetMpegGameProfile(&runtime);
    const auto before = runtime.mem.ram;
    sceMpegCreate(ram, &ctx, &runtime);
    CHECK(ctx.gpr[2] == 0u && runtime.mem.ram == before);
    return 0;
}
