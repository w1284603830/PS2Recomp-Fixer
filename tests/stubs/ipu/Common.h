#pragma once
// Minimal recording runtime for compiling the unmodified production IPU stub headlessly.
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <immintrin.h>
#include <utility>
#include <vector>

struct R5900Context
{
    uint32_t pc = 0;
    std::array<uint32_t, 32> gpr{};
    int32_t result = -99;
};
inline void setReturnS32(R5900Context *ctx, int32_t value) { ctx->result = value; }
#define SET_GPR_U32(ctx, index, value) ((ctx)->gpr[index] = (value))

enum class GuestInvocationKind { HleCall };
struct GuestInvocation
{
    GuestInvocationKind kind{};
    R5900Context context;
    std::function<void(const R5900Context &, R5900Context &)> onComplete;
};
struct InvocationRequested {};
class EeScheduler
{
public:
    GuestInvocation pending;
    int binds = 0;
    void bindMainContextForSyscall(R5900Context &, uint8_t *) { ++binds; }
    [[noreturn]] void invokeCurrent(GuestInvocation invocation)
    {
        pending = std::move(invocation);
        throw InvocationRequested{};
    }
};

class PS2Memory
{
public:
    bool ready = true;
    bool canInitialize = true;
    std::array<uint8_t, 128> ram{};
    std::vector<std::pair<uint32_t, uint32_t>> writes32;
    std::vector<std::pair<uint32_t, std::array<uint8_t, 16>>> writes128;
    uint8_t *getRDRAM() { return ready ? ram.data() : nullptr; }
    bool initialize() { ready = canInitialize; return ready; }
    void write32(uint32_t address, uint32_t value) { writes32.emplace_back(address, value); }
    void write128(uint32_t address, __m128i value)
    {
        std::array<uint8_t, 16> bytes;
        _mm_storeu_si128(reinterpret_cast<__m128i *>(bytes.data()), value);
        writes128.emplace_back(address, bytes);
    }
};

namespace ps2_stubs { void resetIpuInitConfig(); bool hasValidIpuInitConfig(); }

class PS2Runtime
{
public:
    struct IoPaths { std::filesystem::path cdImage; };
    PS2Memory mem;
    EeScheduler scheduler;
    bool canSync = true;
    uint32_t registeredHelper = 0;
    std::vector<uint32_t> loads;
    PS2Memory &memory() { return mem; }
    EeScheduler &eeScheduler() { return scheduler; }
    bool syncCoreSubsystems() { return canSync; }
    bool hasFunction(uint32_t address) { return address == registeredHelper; }
    __m128i Load128(uint8_t *, R5900Context *, uint32_t address)
    {
        loads.push_back(address);
        return _mm_set1_epi32(static_cast<int>(address));
    }
    static IoPaths getIoPaths() { return {}; }
    static void setIoPaths(const IoPaths &) {}
    bool initialize(const char *) { std::fprintf(stderr, "[test-runtime] initialize\n"); return true; }
    bool loadELF(const std::string &) { ps2_stubs::resetIpuInitConfig(); return true; }
    void run() { std::fprintf(stderr, "[test-runtime] ipu=%d\n", ps2_stubs::hasValidIpuInitConfig()); }
};
