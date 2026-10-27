#pragma once

#include "ps2x/iop/iop_host.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class PS2Runtime;

// [DoD] Test launcher audio trace (DOD_AUDIO_TRACE=<folder>, ps2_iop_host.cpp): on, and the F9 save of the last 60 s
// (window thread; returns the saved folder's name, empty when the trace is off).
bool dodAudioTraceEnabled();
std::string dodAudioTraceSave();
void dodAudioTraceNote(const char *text); // an event in the trace's log (any thread; nothing when the trace is off)
struct R5900Context;

class PS2IopHostAdapter final : public ps2x::iop::IopHost
{
public:
    class CallScope
    {
    public:
        CallScope() = default;
        CallScope(PS2IopHostAdapter &owner, R5900Context *context, uint8_t *rdram);
        ~CallScope();

        CallScope(const CallScope &) = delete;
        CallScope &operator=(const CallScope &) = delete;
        CallScope(CallScope &&other) noexcept;
        CallScope &operator=(CallScope &&other) noexcept;

        [[nodiscard]] uint64_t token() const { return m_token; }

    private:
        void release();

        std::unique_lock<std::recursive_mutex> m_lock;
        PS2IopHostAdapter *m_owner = nullptr;
        R5900Context *m_previousContext = nullptr;
        uint8_t *m_previousRdram = nullptr;
        uint64_t m_previousToken = 0;
        uint64_t m_token = 0;
    };

    explicit PS2IopHostAdapter(PS2Runtime &runtime);
    ~PS2IopHostAdapter() override;

    [[nodiscard]] CallScope enterCall(R5900Context *context,
                                      uint8_t *rdram = nullptr);

    bool readGuest(uint32_t address, void *destination, size_t size) const override;
    bool writeGuest(uint32_t address, const void *source, size_t size) override;
    bool zeroGuest(uint32_t address, size_t size) override;
    bool normalizeGuestAddress(uint32_t address, uint32_t &normalized) const override;
    bool readIopMemory(uint32_t address, void *destination, size_t size) const override;
    bool writeIopMemory(uint32_t address, const void *source, size_t size) override;
    bool zeroIopMemory(uint32_t address, size_t size) override;
    bool normalizeIopAddress(uint32_t address, uint32_t &normalized) const override;
    uint32_t allocateIopHandle(ps2x::iop::IopHandleKind kind) override;
    uint32_t allocateGuest(uint32_t size, uint32_t alignment) override;
    void freeGuest(uint32_t address) override;

    void audioCommand(uint32_t sid,
                      uint32_t function,
                      ps2x::iop::GuestBuffer send,
                      ps2x::iop::GuestBuffer receive) override;
    void audioOutput(const int16_t *samples, size_t frames) override; // [DoD] emulated SPU2 (ran-j/PS2Recomp#308)

    std::string hostPath(ps2x::iop::HostPathKind kind) const override;
    std::string translateGuestPath(std::string_view path) const override;
    uint64_t openHostFile(std::string_view path) override;
    bool hostFileSize(uint64_t handle, uint64_t &size) const override;
    bool readHostFile(uint64_t handle,
                      uint64_t offset,
                      void *destination,
                      size_t size,
                      size_t &bytesRead) override;
    void closeHostFile(uint64_t handle) override;

    int32_t memoryCard(const ps2x::iop::MemoryCardRequest &request) override;

    bool hasGuestFunction(uint32_t address) const override;
    bool invokeGuestFunction(uint64_t callToken,
                             uint32_t address,
                             uint32_t a0,
                             uint32_t a1,
                             uint32_t a2,
                             uint32_t a3,
                             uint32_t *resultAddress) override;
    bool sendSifCommand(uint32_t commandId, const void *packet, size_t packetSize) override;
    bool lookupEeRpcServer(uint32_t sid, uint32_t &server, uint32_t &buff, uint32_t &cbuff) const override;

    void log(ps2x::iop::LogLevel level, std::string_view message) override;

    // [DoD] Threaded IOP (KillzoneRecomp patch 0015): sendSifCommand queues instead of touching the EE scheduler;
    // drainPosted() (EE thread) applies the queue in order. See ps2_iop_post.h.
    void setPostSifCommands(bool enable);
    void drainPosted();

private:
    friend class CallScope;

    bool guestRange(uint32_t address, size_t size, uint8_t *&begin) const;

    struct PostedSifCommand
    {
        uint32_t commandId = 0;
        std::vector<uint8_t> packet;
    };

    PS2Runtime &m_runtime;
    std::atomic<bool> m_postSifCommands{false};
    std::mutex m_postMutex;
    std::vector<PostedSifCommand> m_posted;
    bool m_postWakeQueued = false;
    std::recursive_mutex m_callMutex;
    R5900Context *m_activeContext = nullptr;
    uint8_t *m_activeRdram = nullptr;
    uint64_t m_activeToken = 0;
    uint64_t m_nextToken = 1;
    struct HostFile
    {
        std::FILE *stream = nullptr;
        uint64_t size = 0u;
    };
    mutable std::mutex m_hostFileMutex;
    std::unordered_map<uint64_t, HostFile> m_hostFiles;
    uint64_t m_nextHostFileHandle = 1u;
};
