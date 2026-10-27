#pragma once

#include "ps2x/iop/iop_host.h"
#include "ps2x/iop/iop_types.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ps2x::iop
{
    // [DoD] True on the thread that runs the IOP in threaded mode (KillzoneRecomp patch 0015).
    [[nodiscard]] bool onIopThread() noexcept;

    class IopSubsystem
    {
    public:
        explicit IopSubsystem(IopHost &host);
        ~IopSubsystem();

        IopSubsystem(const IopSubsystem &) = delete;
        IopSubsystem &operator=(const IopSubsystem &) = delete;
        IopSubsystem(IopSubsystem &&) noexcept;
        IopSubsystem &operator=(IopSubsystem &&) noexcept;

        void reset();

        [[nodiscard]] ModuleLoadResult loadModule(std::string_view path, const void *arguments = nullptr, uint32_t argumentSize = 0);
        [[nodiscard]] ModuleLoadResult loadModuleBuffer(uint32_t guestAddress, const void *arguments = nullptr, uint32_t argumentSize = 0);
        [[nodiscard]] bool stopModule(int32_t moduleId, int32_t *result = nullptr);
        void runEeCycles(uint64_t eeCycles) noexcept;

        [[nodiscard]] RpcAbi selectRpcAbi(const RpcAbiRequest &request) const;
        [[nodiscard]] bool canBindRpc(uint32_t sid) const noexcept;
        [[nodiscard]] RpcResult handleRpc(const RpcRequest &request);
        void onSifTransfer(const SifTransfer &transfer);
        // [DoD] Disc change: runs hostUpdate (the host switches its CdImage path) while the IOP is held, then drops the
        // cdvd's open image so its next read opens the new disc.
        void changeMedia(const std::function<void()> &hostUpdate);

        // Physical IOP RAM access shared by the emulator, SIF DMA, and HLE services. Addresses are IOP addresses.
        [[nodiscard]] uint32_t allocateMemory(uint32_t size, uint32_t alignment = 16u);
        [[nodiscard]] bool freeMemory(uint32_t address);
        [[nodiscard]] bool readMemory(uint32_t address, void *destination, size_t size) const;
        [[nodiscard]] bool writeMemory(uint32_t address, const void *source, size_t size);
        [[nodiscard]] bool zeroMemory(uint32_t address, size_t size);
        [[nodiscard]] bool isMemoryRange(uint32_t address, size_t size) const;

        [[nodiscard]] DebugSnapshot debugSnapshot() const;

        // ---- [DoD] Threaded mode (KillzoneRecomp patch 0015) ---------------------------------------------------------
        // enableThread(true) serialises every public entry point above with one recursive lock and lets a host thread
        // run the IOP off the caller's thread. The thread starts at the first publishEeCycles(); it consumes the
        // published EE cycles (IOP time = EE / 8, as runEeCycles) in chunks and never runs ahead of them. runEeCycles()
        // is not used while the thread runs. Inside handleRpc/loadModule/... the calling (EE) thread runs the IOP
        // while it holds the lock, as before; IopHost callbacks made on the IOP thread must be thread-safe.
        void enableThread(bool enable);
        [[nodiscard]] bool threadEnabled() const noexcept;
        void publishEeCycles(uint64_t totalEeCycles) noexcept;

    private:
        class Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
