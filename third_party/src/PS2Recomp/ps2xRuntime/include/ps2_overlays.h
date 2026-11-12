#pragma once
// Run-time code overlays: code images a game copies into EE RAM while it runs (stage, enemy or menu modules).
//
// Each image is recompiled on its own (ps2_recomp with an [overlay] section, see docs/GAME_PORTING.md). Its
// generated *_overlay.cpp describes the image: load address, size, a signature (bytes at a fixed offset from the
// load address that identify this image while it is resident) and the image's {guest address, recompiled
// function} table. PS2Runtime::lookupFunction / hasFunction consult the registry for addresses inside a
// registered image and dispatch through whichever image's signature is in EE RAM at that moment, so several
// images can share one load address.
//
// Disabled by default: with nothing registered the dispatch hot path costs one subtract and compare and behaves
// exactly as before. PS2Runtime::loadELF clears the registry. A port registers its images AFTER loadELF and
// before run() (installCatalog() or the generated ps2x_register_overlay_<name>() functions). Registration and
// clear() must not race guest execution; lookups are safe from any number of guest threads.
//
// Load addresses, sizes and signatures come from each port's own configuration; this file holds no game data.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

struct R5900Context;
class PS2Runtime;

namespace ps2_overlays
{
    // Same type as PS2Runtime::RecompiledFunction (checked in ps2_runtime.cpp).
    using Function = void (*)(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

    constexpr uint32_t kGuestRamSize = 32u * 1024u * 1024u; // equals PS2_RAM_SIZE (checked in ps2_runtime.cpp)
    constexpr uint32_t kMaxSignatureSize = 256u;

    struct Entry
    {
        uint32_t address;
        Function function;
    };

    struct Image
    {
        const char *name = nullptr;         // unique; used in logs and diagnostics
        uint32_t loadAddress = 0u;          // EE RAM address the game copies the image to (physical, < 32 MiB)
        uint32_t imageSize = 0u;            // bytes from loadAddress that belong to the image
        uint32_t signatureOffset = 0u;      // relative to loadAddress; signature must lie inside the image
        const uint8_t *signature = nullptr; // bytes present at loadAddress + signatureOffset while resident
        uint32_t signatureSize = 0u;        // 1..kMaxSignatureSize
        const Entry *entries = nullptr;     // ascending, unique, 4-byte aligned, inside the image
        uint32_t entryCount = 0u;           // at least one
    };

    enum class Status
    {
        Ok,
        InvalidImage,       // null/empty field, size or signature out of range, image beyond EE RAM
        BadEntries,         // unsorted, duplicate, unaligned, null or outside the image
        DuplicateName,      // another registered image already uses this name
        AmbiguousSignature, // an image at the same load address could match the same RAM contents
    };
    const char *describe(Status status);

    // Copies the description (name, signature and entries); the caller's storage may go away afterwards.
    // Two images at one load address must have signatures that overlap in at least one byte where they differ,
    // otherwise some RAM contents would match both and the dispatch would be ambiguous.
    Status registerImage(const Image &image);
    void clear();
    size_t registeredCount();

    enum class Resolution
    {
        NotCovered, // no resident registered image contains the address: use the main function table
        Found,      // a resident image contains the address and has a function there
        Missing,    // a resident image contains the address but has no function there (never the main table)
    };
    struct Lookup
    {
        Resolution resolution = Resolution::NotCovered;
        Function function = nullptr;
        const char *image = nullptr; // name of the resident image (Found / Missing); valid until clear()
    };

    namespace detail
    {
        extern std::atomic<uint32_t> g_coverLow;  // lowest registered load address
        extern std::atomic<uint32_t> g_coverSpan; // 0 while nothing is registered
    }

    // True when the address may lie in a registered image. The only cost on the dispatch hot path.
    inline bool mayCover(uint32_t address) noexcept
    {
        return address - detail::g_coverLow.load(std::memory_order_relaxed) <
               detail::g_coverSpan.load(std::memory_order_relaxed);
    }

    // rdram: the 32 MiB EE RAM buffer. Images at higher load addresses are tried first: a smaller image loaded
    // inside a larger one's range replaces that part of it.
    Lookup resolve(const uint8_t *rdram, uint32_t address);

    // Name of the image resident at a registered load address, nullptr when none matches (or no such area).
    const char *residentImage(const uint8_t *rdram, uint32_t loadAddress);
    // "0x00D20000=<name> 0x00D00000=-" for diagnostics; empty when nothing is registered.
    std::string describeResident(const uint8_t *rdram);

    // Passive catalog: generated overlay sources add their image here from a static initializer. Nothing in the
    // catalog is active until the port calls installCatalog() after loadELF. The catalog is never cleared.
    struct CatalogRegistrar
    {
        explicit CatalogRegistrar(const Image &image);
    };
    size_t catalogSize();
    struct InstallReport
    {
        size_t installed = 0u;
        size_t failed = 0u; // each failure is printed with its reason
    };
    InstallReport installCatalog();
}
