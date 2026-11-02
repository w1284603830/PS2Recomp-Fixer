#pragma once
// Recording host services only; tests compile the complete production MPEG.cpp.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

constexpr uint32_t PS2_RAM_SIZE = 32u * 1024u * 1024u;
constexpr uint32_t PS2_RAM_MASK = PS2_RAM_SIZE - 1u;
constexpr int KE_OK = 0, KE_WAIT_DELETE = -1;
struct R5900Context { uint32_t pc = 0; std::array<uint32_t, 32> gpr{}; std::array<float, 32> f{}; };
inline uint32_t getRegU32(const R5900Context *ctx, int reg) { return ctx->gpr[reg]; }
inline void setReturnU32(R5900Context *ctx, uint32_t value) { ctx->gpr[2] = value; }
inline void setReturnS32(R5900Context *ctx, int32_t value) { ctx->gpr[2] = static_cast<uint32_t>(value); }
inline uint8_t *getMemPtr(uint8_t *ram, uint32_t address) { return ram ? ram + (address & PS2_RAM_MASK) : nullptr; }
inline const uint8_t *getConstMemPtr(const uint8_t *ram, uint32_t address) { return ram ? ram + (address & PS2_RAM_MASK) : nullptr; }
inline uint32_t readTestWord(const uint8_t *ram, uint32_t address)
{
    uint32_t value; std::memcpy(&value, getConstMemPtr(ram, address), 4); return value;
}
#define FAST_READ32(address) readTestWord(rdram, address)
#define SET_GPR_U32(ctx, reg, value) ((ctx)->gpr[reg] = (value))
#define PS2_IF_AGRESSIVE_LOGS(...) do {} while (false)
#define TODO_NAMED(...) do {} while (false)

enum class GuestInvocationKind { HleCall, RpcCallback };
enum class EeWaitReason { Mpeg };
struct GuestInvocation
{
    GuestInvocationKind kind{};
    R5900Context context;
    std::function<void(const R5900Context &, R5900Context &)> onComplete;
};
class EeScheduler
{
public:
    int calls = 0;
    void bindMainContextForSyscall(R5900Context &, uint8_t *) { ++calls; }
    void queueInvocation(GuestInvocation) { ++calls; }
    void invokeCurrent(GuestInvocation) { throw 1; }
    uint64_t currentVSyncTick() const { return 0; }
    void completeExternalWait(uint32_t, uint32_t, int) { ++calls; }
    void waitVSync(uint64_t, int, std::function<void(R5900Context &)>) { throw 2; }
    void waitExternal(EeWaitReason, uint32_t, uint32_t, std::function<void(R5900Context &)>) { throw 3; }
};
class PS2Memory
{
public:
    std::vector<uint8_t> ram = std::vector<uint8_t>(PS2_RAM_SIZE);
    std::vector<std::pair<uint32_t, uint32_t>> writes;
    uint8_t *getRDRAM() { return ram.data(); }
    void write32(uint32_t address, uint32_t value) { writes.emplace_back(address, value); }
    void writeIORegister(uint32_t address, uint32_t value) { write32(address, value); }
    void processPendingTransfers() {}
};
class PS2Runtime
{
public:
    using RecompiledFunction = void (*)(uint8_t *, R5900Context *, PS2Runtime *);
    PS2Memory mem;
    EeScheduler scheduler;
    std::map<uint32_t, RecompiledFunction> functions;
    PS2Memory &memory() { return mem; }
    EeScheduler &eeScheduler() { return scheduler; }
    uint32_t guestMalloc(uint32_t, uint32_t) { return 0x180000u; }
    void guestFree(uint32_t) {}
    bool hasFunction(uint32_t address) { return functions.count(address); }
    RecompiledFunction lookupFunction(uint32_t address) { return functions.count(address) ? functions[address] : nullptr; }
    bool replaceFunction(uint32_t address, RecompiledFunction function) { functions[address] = function; return true; }
};
