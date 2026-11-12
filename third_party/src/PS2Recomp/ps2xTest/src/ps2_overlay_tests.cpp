// Run-time code overlay registry (ps2_overlays.h), its PS2Runtime dispatch, and ps2_recomp's [overlay] output.
// Every address here is synthetic test data, not taken from any game.
#include "MiniTest.h"
#include "ps2_overlays.h"
#include "ps2_runtime.h"
#include "ps2recomp/config_manager.h"
#include "ps2recomp/ps2_recompiler.h"
#include "ps2recomp/types.h"

#include <elfio/elfio.hpp>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ps2recomp;

namespace
{
    constexpr uint32_t kBase = 0x00400000u;

    void overlayFnA(uint8_t *, R5900Context *, PS2Runtime *) {}
    void overlayFnB(uint8_t *, R5900Context *, PS2Runtime *) {}
    void overlayFnC(uint8_t *, R5900Context *, PS2Runtime *) {}
    void mainFn(uint8_t *, R5900Context *, PS2Runtime *) {}

    const uint8_t kSigA[] = {'O', 'V', 'L', 'A'};
    const uint8_t kSigB[] = {'O', 'V', 'L', 'B'};
    const ps2_overlays::Entry kEntriesA[] = {{kBase + 0x10u, &overlayFnA}, {kBase + 0x40u, &overlayFnA}};
    const ps2_overlays::Entry kEntriesB[] = {{kBase + 0x10u, &overlayFnB}};

    ps2_overlays::Image makeImage(const char *name, uint32_t loadAddress, uint32_t size, const uint8_t *signature,
                                  uint32_t signatureSize, const ps2_overlays::Entry *entries, uint32_t count,
                                  uint32_t signatureOffset = 0u)
    {
        ps2_overlays::Image image;
        image.name = name;
        image.loadAddress = loadAddress;
        image.imageSize = size;
        image.signatureOffset = signatureOffset;
        image.signature = signature;
        image.signatureSize = signatureSize;
        image.entries = entries;
        image.entryCount = count;
        return image;
    }

    void place(std::vector<uint8_t> &ram, uint32_t address, const uint8_t *bytes, size_t size)
    {
        std::memcpy(ram.data() + address, bytes, size);
    }

    // Passive catalog entry: must stay inactive until a test calls installCatalog().
    const uint8_t kCatalogSig[] = {'C', 'A', 'T', '0'};
    const ps2_overlays::Entry kCatalogEntries[] = {{0x00500000u + 0x20u, &overlayFnC}};
    const ps2_overlays::Image kCatalogImage = {
        "test_catalog_image", 0x00500000u, 0x100u, 0u, kCatalogSig, 4u, kCatalogEntries, 1u};
    const ps2_overlays::CatalogRegistrar g_testCatalog(kCatalogImage);

    std::string readText(const std::filesystem::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    std::filesystem::path uniqueTempDir(const std::string &stem)
    {
        const auto suffix = std::to_string(
            static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / (stem + "-" + suffix);
        std::filesystem::create_directories(dir);
        return dir;
    }

    void putWord(std::vector<char> &image, size_t offset, uint32_t word)
    {
        std::memcpy(image.data() + offset, &word, sizeof(word));
    }

    // 16-byte header (8-byte signature "TESTOVL1" + padding), then code at load + 0x10:
    //   0x10 A: addiu sp,-16 / sw ra / jal B / nop / jal 0x100000 (main ELF) / nop / lw ra / jr ra / addiu sp,16
    //   0x34    nop padding
    //   0x38 B: jr ra / addiu v0, zero, 7
    std::vector<char> makeRawOverlayImage()
    {
        std::vector<char> image(0x40u, 0);
        std::memcpy(image.data(), "TESTOVL1", 8u);
        putWord(image, 0x10u, 0x27BDFFF0u);
        putWord(image, 0x14u, 0xAFBF0000u);
        putWord(image, 0x18u, 0x0C000000u | ((kBase + 0x38u) >> 2));
        putWord(image, 0x1Cu, 0x00000000u);
        putWord(image, 0x20u, 0x0C000000u | (0x00100000u >> 2));
        putWord(image, 0x24u, 0x00000000u);
        putWord(image, 0x28u, 0x8FBF0000u);
        putWord(image, 0x2Cu, 0x03E00008u);
        putWord(image, 0x30u, 0x27BD0010u);
        putWord(image, 0x34u, 0x00000000u);
        putWord(image, 0x38u, 0x03E00008u);
        putWord(image, 0x3Cu, 0x24020007u);
        return image;
    }

    bool writeBytes(const std::filesystem::path &path, const std::vector<char> &bytes)
    {
        std::ofstream file(path, std::ios::binary);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return static_cast<bool>(file);
    }

    bool writeOverlayConfig(const std::filesystem::path &configPath, const std::filesystem::path &input,
                            const std::filesystem::path &output, bool singleFile, const std::string &overlayExtra)
    {
        std::ofstream config(configPath);
        config << "[general]\n";
        config << "input = \"" << input.generic_string() << "\"\n";
        config << "output = \"" << output.generic_string() << "\"\n";
        config << "single_file_output = " << (singleFile ? "true" : "false") << "\n";
        config << "output_worker_threads = 1\n";
        config << "entry_points = [\"ovl_b@0x400038\"]\n\n";
        config << "[overlay]\n";
        config << "name = \"unit_ovl\"\n";
        config << "load_address = 0x400000\n";
        config << "code_offset = 0x10\n";
        config << overlayExtra;
        return static_cast<bool>(config);
    }

    const std::string kSignature8 = "signature_size = 8\n";

    bool recompileOverlay(const std::filesystem::path &configPath)
    {
        PS2Recompiler recompiler(configPath.string());
        if (!recompiler.initialize() || !recompiler.recompile())
            return false;
        recompiler.generateOutput();
        return true;
    }
}

void register_ps2_overlay_tests()
{
    MiniTest::Case("PS2Overlays", [](TestCase &tc)
    {
        tc.Run("nothing registered: no address is covered", [](TestCase &t)
        {
            ps2_overlays::clear();
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            t.Equals(ps2_overlays::registeredCount(), static_cast<size_t>(0), "registry starts empty");
            t.IsFalse(ps2_overlays::mayCover(0u), "address 0 must not be covered");
            t.IsFalse(ps2_overlays::mayCover(kBase + 0x10u), "no range is covered");
            t.IsFalse(ps2_overlays::mayCover(0xFFFFFFFCu), "the top address must not be covered");
            t.IsTrue(ps2_overlays::resolve(ram.data(), kBase + 0x10u).resolution == ps2_overlays::Resolution::NotCovered,
                     "resolve reports NotCovered");
            t.IsTrue(ps2_overlays::describeResident(ram.data()).empty(), "no areas to describe");
        });

        tc.Run("dispatch follows the signature resident in RAM", [](TestCase &t)
        {
            ps2_overlays::clear();
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            t.IsTrue(ps2_overlays::registerImage(makeImage("ovl_a", kBase, 0x100u, kSigA, 4u, kEntriesA, 2u)) ==
                         ps2_overlays::Status::Ok, "image A registers");
            t.IsTrue(ps2_overlays::registerImage(makeImage("ovl_b", kBase, 0x80u, kSigB, 4u, kEntriesB, 1u)) ==
                         ps2_overlays::Status::Ok, "image B shares the load address");
            t.Equals(ps2_overlays::registeredCount(), static_cast<size_t>(2), "two images registered");
            t.IsTrue(ps2_overlays::mayCover(kBase) && ps2_overlays::mayCover(kBase + 0xFCu), "image range covered");
            t.IsFalse(ps2_overlays::mayCover(kBase - 4u) || ps2_overlays::mayCover(kBase + 0x100u),
                      "outside the images is not covered");

            t.IsTrue(ps2_overlays::resolve(ram.data(), kBase + 0x10u).resolution == ps2_overlays::Resolution::NotCovered,
                     "neither signature in RAM: not covered");
            place(ram, kBase, kSigA, 4u);
            ps2_overlays::Lookup a = ps2_overlays::resolve(ram.data(), kBase + 0x10u);
            t.IsTrue(a.resolution == ps2_overlays::Resolution::Found && a.function == &overlayFnA, "A resident: A's function");
            t.IsTrue(a.image != nullptr && std::string(a.image) == "ovl_a", "A is named");
            t.IsTrue(std::string(ps2_overlays::residentImage(ram.data(), kBase)) == "ovl_a", "residentImage reports A");

            place(ram, kBase, kSigB, 4u);
            ps2_overlays::Lookup b = ps2_overlays::resolve(ram.data(), kBase + 0x10u);
            t.IsTrue(b.resolution == ps2_overlays::Resolution::Found && b.function == &overlayFnB, "B resident: B's function");
            t.IsTrue(ps2_overlays::resolve(ram.data(), kBase + 0x40u).resolution == ps2_overlays::Resolution::Missing,
                     "B covers 0x40 but has no function there: A's entry must not leak through");
            place(ram, kBase, kSigA, 4u);
            t.IsTrue(ps2_overlays::resolve(ram.data(), kBase + 0x40u).function == &overlayFnA, "switching back to A");
            t.IsTrue(ps2_overlays::resolve(ram.data(), kBase + 0x12u).resolution == ps2_overlays::Resolution::NotCovered,
                     "unaligned addresses are left to the main table");
            ps2_overlays::clear();
        });

        tc.Run("a resident image owns its whole range", [](TestCase &t)
        {
            ps2_overlays::clear();
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            const ps2_overlays::Entry entries[] = {{kBase + 0x10u, &overlayFnB}};
            t.IsTrue(ps2_overlays::registerImage(makeImage("ovl_b", kBase, 0x80u, kSigB, 4u, entries, 1u)) ==
                         ps2_overlays::Status::Ok, "image registers");
            place(ram, kBase, kSigB, 4u);
            const ps2_overlays::Lookup inside = ps2_overlays::resolve(ram.data(), kBase + 0x40u);
            t.IsTrue(inside.resolution == ps2_overlays::Resolution::Missing && inside.function == nullptr,
                     "an address inside the image without an entry is Missing");
            t.IsTrue(ps2_overlays::resolve(ram.data(), kBase + 0x80u).resolution == ps2_overlays::Resolution::NotCovered,
                     "the first byte after the image is not covered");
            ps2_overlays::clear();
        });

        tc.Run("a smaller image inside a larger one wins its own range", [](TestCase &t)
        {
            ps2_overlays::clear();
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            const uint32_t inner = kBase + 0x10000u;
            const ps2_overlays::Entry outerEntries[] = {{kBase + 0x100u, &overlayFnA}, {inner + 0x100u, &overlayFnA},
                                                        {inner + 0x2000u, &overlayFnA}};
            const ps2_overlays::Entry innerEntries[] = {{inner + 0x100u, &overlayFnB}};
            t.IsTrue(ps2_overlays::registerImage(makeImage("outer", kBase, 0x20000u, kSigA, 4u, outerEntries, 3u)) ==
                         ps2_overlays::Status::Ok, "outer image registers");
            t.IsTrue(ps2_overlays::registerImage(makeImage("inner", inner, 0x1000u, kSigB, 4u, innerEntries, 1u)) ==
                         ps2_overlays::Status::Ok, "inner image at a different load address registers");
            place(ram, kBase, kSigA, 4u);
            t.IsTrue(ps2_overlays::resolve(ram.data(), inner + 0x100u).function == &overlayFnA,
                     "inner not resident: the outer image's code runs");
            place(ram, inner, kSigB, 4u);
            t.IsTrue(ps2_overlays::resolve(ram.data(), inner + 0x100u).function == &overlayFnB,
                     "inner resident: the higher load address wins");
            t.IsTrue(ps2_overlays::resolve(ram.data(), kBase + 0x100u).function == &overlayFnA,
                     "below the inner image the outer image still dispatches");
            t.IsTrue(ps2_overlays::resolve(ram.data(), inner + 0x2000u).function == &overlayFnA,
                     "beyond the inner image's size the outer image dispatches");
            const std::string described = ps2_overlays::describeResident(ram.data());
            t.IsTrue(described == "0x00410000=inner 0x00400000=outer", "describeResident lists areas: " + described);
            ps2_overlays::clear();
        });

        tc.Run("invalid descriptions are rejected", [](TestCase &t)
        {
            ps2_overlays::clear();
            using ps2_overlays::Status;
            const uint8_t big[257] = {};
            const ps2_overlays::Entry unsorted[] = {{kBase + 0x20u, &overlayFnA}, {kBase + 0x10u, &overlayFnA}};
            const ps2_overlays::Entry duplicate[] = {{kBase + 0x10u, &overlayFnA}, {kBase + 0x10u, &overlayFnA}};
            const ps2_overlays::Entry unaligned[] = {{kBase + 0x12u, &overlayFnA}};
            const ps2_overlays::Entry outside[] = {{kBase + 0x100u, &overlayFnA}};
            const ps2_overlays::Entry below[] = {{kBase - 4u, &overlayFnA}};
            const ps2_overlays::Entry nullFn[] = {{kBase + 0x10u, nullptr}};
            t.IsTrue(ps2_overlays::registerImage(makeImage(nullptr, kBase, 0x100u, kSigA, 4u, kEntriesA, 2u)) == Status::InvalidImage, "null name");
            t.IsTrue(ps2_overlays::registerImage(makeImage("", kBase, 0x100u, kSigA, 4u, kEntriesA, 2u)) == Status::InvalidImage, "empty name");
            t.IsTrue(ps2_overlays::registerImage(makeImage("x", kBase, 0x100u, kSigA, 0u, kEntriesA, 2u)) == Status::InvalidImage, "empty signature");
            t.IsTrue(ps2_overlays::registerImage(makeImage("x", kBase, 0x200u, big, 257u, kEntriesA, 2u)) == Status::InvalidImage, "signature over 256 bytes");
            t.IsTrue(ps2_overlays::registerImage(makeImage("x", kBase, 0x100u, nullptr, 4u, kEntriesA, 2u)) == Status::InvalidImage, "null signature");
            t.IsTrue(ps2_overlays::registerImage(makeImage("x", kBase, 0x100u, kSigA, 4u, kEntriesA, 2u, 0xFDu)) == Status::InvalidImage, "signature past the image end");
            t.IsTrue(ps2_overlays::registerImage(makeImage("x", kBase, 0u, kSigA, 4u, kEntriesA, 2u)) == Status::InvalidImage, "empty image");
            t.IsTrue(ps2_overlays::registerImage(makeImage("x", PS2_RAM_SIZE - 0x80u, 0x100u, kSigA, 4u, kEntriesA, 2u)) == Status::InvalidImage, "image beyond EE RAM");
            t.IsTrue(ps2_overlays::registerImage(makeImage("x", kBase, 0x100u, kSigA, 4u, nullptr, 2u)) == Status::InvalidImage, "null entries");
            t.IsTrue(ps2_overlays::registerImage(makeImage("x", kBase, 0x100u, kSigA, 4u, kEntriesA, 0u)) == Status::InvalidImage, "no entries");
            t.IsTrue(ps2_overlays::registerImage(makeImage("x", kBase, 0x100u, kSigA, 4u, unsorted, 2u)) == Status::BadEntries, "unsorted entries");
            t.IsTrue(ps2_overlays::registerImage(makeImage("x", kBase, 0x100u, kSigA, 4u, duplicate, 2u)) == Status::BadEntries, "duplicate entries");
            t.IsTrue(ps2_overlays::registerImage(makeImage("x", kBase, 0x100u, kSigA, 4u, unaligned, 1u)) == Status::BadEntries, "unaligned entry");
            t.IsTrue(ps2_overlays::registerImage(makeImage("x", kBase, 0x100u, kSigA, 4u, outside, 1u)) == Status::BadEntries, "entry past the image");
            t.IsTrue(ps2_overlays::registerImage(makeImage("x", kBase, 0x100u, kSigA, 4u, below, 1u)) == Status::BadEntries, "entry below the image");
            t.IsTrue(ps2_overlays::registerImage(makeImage("x", kBase, 0x100u, kSigA, 4u, nullFn, 1u)) == Status::BadEntries, "null function");
            t.Equals(ps2_overlays::registeredCount(), static_cast<size_t>(0), "nothing registered after rejections");
            t.IsFalse(ps2_overlays::mayCover(kBase), "rejections publish no range");
            ps2_overlays::clear();
        });

        tc.Run("ambiguous signatures and duplicate names are rejected", [](TestCase &t)
        {
            ps2_overlays::clear();
            using ps2_overlays::Status;
            const uint8_t sigAB[] = {'O', 'V', 'L', 'A', 'B'};
            const uint8_t sigTail[] = {'Z', 'Z'};
            t.IsTrue(ps2_overlays::registerImage(makeImage("ovl_a", kBase, 0x100u, kSigA, 4u, kEntriesA, 2u)) == Status::Ok, "A registers");
            t.IsTrue(ps2_overlays::registerImage(makeImage("ovl_a", kBase + 0x1000u, 0x100u, kSigB, 4u, kEntriesB, 0u)) == Status::InvalidImage,
                     "validation runs before the duplicate-name check");
            const ps2_overlays::Entry other[] = {{kBase + 0x1010u, &overlayFnB}};
            t.IsTrue(ps2_overlays::registerImage(makeImage("ovl_a", kBase + 0x1000u, 0x100u, kSigB, 4u, other, 1u)) == Status::DuplicateName,
                     "names are unique across load addresses");
            t.IsTrue(ps2_overlays::registerImage(makeImage("same", kBase, 0x100u, kSigA, 4u, kEntriesB, 1u)) == Status::AmbiguousSignature,
                     "an identical signature at the same load address");
            t.IsTrue(ps2_overlays::registerImage(makeImage("longer", kBase, 0x100u, sigAB, 5u, kEntriesB, 1u)) == Status::AmbiguousSignature,
                     "a signature that extends another one matches the same RAM");
            t.IsTrue(ps2_overlays::registerImage(makeImage("apart", kBase, 0x100u, sigTail, 2u, kEntriesB, 1u, 0x20u)) == Status::AmbiguousSignature,
                     "non-overlapping signature ranges can both match");
            t.IsTrue(ps2_overlays::registerImage(makeImage("ovl_b", kBase, 0x80u, kSigB, 4u, kEntriesB, 1u)) == Status::Ok,
                     "a signature differing in an overlapping byte registers");
            t.Equals(ps2_overlays::registeredCount(), static_cast<size_t>(2), "only the valid images remain");
            ps2_overlays::clear();
        });

        tc.Run("registration copies the description", [](TestCase &t)
        {
            ps2_overlays::clear();
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            {
                std::string name = "transient";
                uint8_t signature[] = {1u, 2u, 3u, 4u};
                ps2_overlays::Entry entries[] = {{kBase + 0x10u, &overlayFnC}};
                t.IsTrue(ps2_overlays::registerImage(makeImage(name.c_str(), kBase, 0x40u, signature, 4u, entries, 1u)) ==
                             ps2_overlays::Status::Ok, "image registers from temporary storage");
                name.assign("overwritten");
                std::memset(signature, 0xEE, sizeof(signature));
                entries[0].function = &overlayFnA;
            }
            const uint8_t resident[] = {1u, 2u, 3u, 4u};
            place(ram, kBase, resident, 4u);
            const ps2_overlays::Lookup lookup = ps2_overlays::resolve(ram.data(), kBase + 0x10u);
            t.IsTrue(lookup.function == &overlayFnC, "the copied entry table is used");
            t.IsTrue(lookup.image != nullptr && std::string(lookup.image) == "transient", "the copied name is used");
            ps2_overlays::clear();
        });

        tc.Run("clear empties the registry", [](TestCase &t)
        {
            ps2_overlays::clear();
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            t.IsTrue(ps2_overlays::registerImage(makeImage("ovl_a", kBase, 0x100u, kSigA, 4u, kEntriesA, 2u)) ==
                         ps2_overlays::Status::Ok, "image registers");
            place(ram, kBase, kSigA, 4u);
            ps2_overlays::clear();
            t.Equals(ps2_overlays::registeredCount(), static_cast<size_t>(0), "registry empty after clear");
            t.IsFalse(ps2_overlays::mayCover(kBase + 0x10u), "range disabled after clear");
            t.IsTrue(ps2_overlays::resolve(ram.data(), kBase + 0x10u).resolution == ps2_overlays::Resolution::NotCovered,
                     "the resident signature no longer dispatches");
            t.IsTrue(ps2_overlays::registerImage(makeImage("ovl_a", kBase, 0x100u, kSigA, 4u, kEntriesA, 2u)) ==
                         ps2_overlays::Status::Ok, "the same name registers again after clear");
            ps2_overlays::clear();
        });

        tc.Run("the catalog stays passive until installed", [](TestCase &t)
        {
            ps2_overlays::clear();
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            place(ram, 0x00500000u, kCatalogSig, 4u);
            t.IsTrue(ps2_overlays::catalogSize() >= 1u, "the test image is in the catalog");
            t.Equals(ps2_overlays::registeredCount(), static_cast<size_t>(0), "a catalog entry is not registered by itself");
            t.IsFalse(ps2_overlays::mayCover(0x00500020u), "a catalog entry covers nothing before installation");
            const ps2_overlays::InstallReport report = ps2_overlays::installCatalog();
            t.Equals(report.installed, ps2_overlays::catalogSize(), "every catalog image installs");
            t.Equals(report.failed, static_cast<size_t>(0), "no catalog image fails");
            t.IsTrue(ps2_overlays::resolve(ram.data(), 0x00500020u).function == &overlayFnC, "installed image dispatches");
            const ps2_overlays::InstallReport again = ps2_overlays::installCatalog();
            t.Equals(again.installed, static_cast<size_t>(0), "a second installation registers nothing");
            t.Equals(again.failed, ps2_overlays::catalogSize(), "a second installation reports the duplicates");
            ps2_overlays::clear();
        });
    });

    MiniTest::Case("PS2RuntimeOverlays", [](TestCase &tc)
    {
        tc.Run("lookupFunction and hasFunction dispatch through the resident image", [](TestCase &t)
        {
            ps2_overlays::clear();
            PS2Runtime runtime;
            t.IsTrue(runtime.memory().initialize(), "runtime memory initialize should succeed");
            uint8_t *rdram = runtime.memory().getRDRAM();
            const uint32_t covered = kBase + 0x10u;
            const uint32_t uncovered = kBase + 0x20u;
            t.IsTrue(runtime.registerFunction(covered, &mainFn) && runtime.registerFunction(uncovered, &mainFn),
                     "main-table functions at the overlay's addresses");
            t.IsTrue(runtime.lookupFunction(covered) == &mainFn, "without overlays the main table is used");

            const ps2_overlays::Entry entries[] = {{covered, &overlayFnA}};
            t.IsTrue(ps2_overlays::registerImage(makeImage("ovl_a", kBase, 0x100u, kSigA, 4u, entries, 1u)) ==
                         ps2_overlays::Status::Ok, "image registers");
            t.IsTrue(runtime.lookupFunction(covered) == &mainFn, "signature absent: the main table still dispatches");
            t.IsTrue(runtime.hasFunction(uncovered), "signature absent: main-table function exists");

            std::memcpy(rdram + kBase, kSigA, sizeof(kSigA));
            t.IsTrue(runtime.lookupFunction(covered) == &overlayFnA, "signature resident: the overlay dispatches");
            t.IsTrue(runtime.hasFunction(covered), "hasFunction sees the overlay entry");
            t.IsFalse(runtime.hasFunction(uncovered), "the resident image has no function where the main ELF had one");
            PS2Runtime::RecompiledFunction trap = runtime.lookupFunction(uncovered);
            t.IsTrue(trap != nullptr && trap != &mainFn, "lookup returns the missing-function trap, not stale main code");

            std::memcpy(rdram + kBase, kSigB, sizeof(kSigB));
            t.IsTrue(runtime.lookupFunction(covered) == &mainFn, "another signature: back to the main table");
            ps2_overlays::clear();
        });

        tc.Run("loadELF clears registered overlays", [](TestCase &t)
        {
            ps2_overlays::clear();
            const PS2Runtime::IoPaths savedPaths = PS2Runtime::getIoPaths();
            PS2Runtime runtime;
            t.IsTrue(ps2_overlays::registerImage(makeImage("ovl_a", kBase, 0x100u, kSigA, 4u, kEntriesA, 2u)) ==
                         ps2_overlays::Status::Ok, "image registers");
            const std::filesystem::path missing = uniqueTempDir("ps2x-overlay-loadelf") / "missing.elf";
            t.IsFalse(runtime.loadELF(missing.string()), "a missing ELF fails to load");
            t.Equals(ps2_overlays::registeredCount(), static_cast<size_t>(0), "loadELF clears the registry first");
            t.IsFalse(ps2_overlays::mayCover(kBase + 0x10u), "no range remains covered");
            PS2Runtime::setIoPaths(savedPaths);
            std::error_code ignored;
            std::filesystem::remove_all(missing.parent_path(), ignored);
        });
    });

    MiniTest::Case("PS2RecompilerOverlays", [](TestCase &tc)
    {
        tc.Run("config manager parses and validates [overlay]", [](TestCase &t)
        {
            const std::filesystem::path dir = uniqueTempDir("ps2recomp-overlay-config");
            auto load = [&](const std::string &overlay) -> RecompilerConfig
            {
                const std::filesystem::path path = dir / "config.toml";
                std::ofstream file(path);
                file << "[general]\ninput = \"image.bin\"\noutput = \"out\"\n\n[overlay]\n" << overlay;
                file.close();
                return ConfigManager(path.string()).loadConfig();
            };
            auto rejects = [&](const std::string &overlay)
            {
                try
                {
                    (void)load(overlay);
                }
                catch (const std::exception &)
                {
                    return true;
                }
                return false;
            };
            const RecompilerConfig config = load("name = \"stage_1\"\nload_address = \"0x00D00000\"\nimage_size = 0x1400\n"
                                                 "signature_offset = 4\nsignature_size = 64\ninput_format = \"raw\"\n"
                                                 "code_offset = 0x40\ncode_size = 0x1000\nbss_size = 0x200\nentry = 0xD00080\n");
            t.IsTrue(config.overlay.enabled, "[overlay] enables overlay output");
            t.Equals(config.overlay.name, std::string("stage_1"), "name parses");
            t.Equals(config.overlay.loadAddress, 0x00D00000u, "load address parses from a string");
            t.Equals(config.overlay.imageSize, 0x1400u, "image size parses");
            t.Equals(config.overlay.signatureOffset, 4u, "signature offset parses");
            t.Equals(config.overlay.signatureSize, 64u, "signature size parses");
            t.IsTrue(config.overlay.inputFormat == OverlayConfig::InputFormat::Raw, "input format parses");
            t.Equals(config.overlay.rawCodeOffset, 0x40u, "code offset parses");
            t.Equals(config.overlay.rawCodeSize, 0x1000u, "code size parses");
            t.Equals(config.overlay.rawBssSize, 0x200u, "bss size parses");
            t.Equals(config.overlay.rawEntry, 0xD00080u, "entry parses");

            const std::string valid = "load_address = 0x400000\nsignature_size = 4\n";
            t.IsTrue(rejects(valid), "a name is required");
            t.IsTrue(rejects("name = \"1abc\"\n" + valid), "a name must not start with a digit");
            t.IsTrue(rejects("name = \"a-b\"\n" + valid), "a name must be an identifier");
            t.IsTrue(rejects("name = \"" + std::string(65, 'a') + "\"\n" + valid), "a name is at most 64 characters");
            t.IsTrue(rejects("name = \"a\"\nsignature_size = 4\n"), "load_address is required");
            t.IsTrue(rejects("name = \"a\"\nload_address = 0x400002\nsignature_size = 4\n"), "load_address is aligned");
            t.IsTrue(rejects("name = \"a\"\nload_address = 0x2000000\nsignature_size = 4\n"), "load_address is inside EE RAM");
            t.IsTrue(rejects("name = \"a\"\nload_address = 0x400000\n"), "signature_size is required");
            t.IsTrue(rejects("name = \"a\"\nload_address = 0x400000\nsignature_size = 0\n"), "signature_size >= 1");
            t.IsTrue(rejects("name = \"a\"\nload_address = 0x400000\nsignature_size = 257\n"), "signature_size <= 256");
            t.IsTrue(rejects("name = \"a\"\n" + valid + "input_format = \"bin\"\n"), "unknown input formats are rejected");
            t.IsTrue(rejects("name = \"a\"\n" + valid + "code_offset = 2\n"), "code_offset is aligned");
            t.IsTrue(rejects("name = \"a\"\nload_address = \"0x40zz\"\nsignature_size = 4\n"), "malformed numbers are rejected");
            t.IsTrue(rejects("name = \"a\"\nload_address = -4\nsignature_size = 4\n"), "negative numbers are rejected");
            t.IsTrue(rejects("name = \"a\"\n" + valid + "imag_size = 0x100\n"), "misspelled keys are rejected");
            t.IsFalse(rejects("name = \"a\"\n" + valid), "the minimal overlay configuration loads");

            std::error_code ignored;
            std::filesystem::remove_all(dir, ignored);
        });

        tc.Run("raw image recompiles into namespaced overlay output", [](TestCase &t)
        {
            const std::filesystem::path dir = uniqueTempDir("ps2recomp-overlay-raw");
            const std::filesystem::path image = dir / "unit.bin";
            const std::filesystem::path out = dir / "out";
            const std::filesystem::path config = dir / "overlay.toml";
            t.IsTrue(writeBytes(image, makeRawOverlayImage()) && writeOverlayConfig(config, image, out, true, kSignature8),
                     "inputs are written");
            t.IsTrue(recompileOverlay(config), "the raw image recompiles");

            t.IsFalse(std::filesystem::exists(out / "register_functions.cpp"), "no dense table for an overlay");
            t.IsFalse(std::filesystem::exists(out / "ps2_recompiled_functions.h"), "no main-ELF header names");
            const std::string header = readText(out / "unit_ovl_functions.h");
            const std::string stubs = readText(out / "unit_ovl_stubs.h");
            const std::string code = readText(out / "unit_ovl_functions.cpp");
            const std::string registration = readText(out / "unit_ovl_overlay.cpp");
            const std::string registrationHeader = readText(out / "unit_ovl_overlay.h");
            t.IsTrue(header.find("namespace ps2x_overlay_unit_ovl {") != std::string::npos, "declarations in the overlay namespace");
            t.IsTrue(header.find("PS2X_OVERLAY_unit_ovl_FUNCTIONS_H") != std::string::npos, "overlay-specific include guard");
            t.IsTrue(stubs.find("namespace ps2x_overlay_unit_ovl {") != std::string::npos, "stub declarations in the namespace");
            t.IsTrue(code.find("#include \"unit_ovl_functions.h\"") != std::string::npos, "sources include their own header by path");
            t.IsTrue(code.find("namespace ps2x_overlay_unit_ovl {") != std::string::npos &&
                         code.find("} // namespace ps2x_overlay_unit_ovl") != std::string::npos, "code in the overlay namespace");
            t.IsTrue(code.find("0x400010") != std::string::npos && code.find("0x400038") != std::string::npos,
                     "both functions are generated");
            t.IsTrue(code.find("dispatchGuestBranch(rdram, ctx, 0x100000u") != std::string::npos,
                     "calls into the main ELF go through runtime dispatch");
            t.IsTrue(registration.find("{0x400010u, &") != std::string::npos &&
                         registration.find("{0x400038u, &") != std::string::npos, "entries register both functions");
            t.IsTrue(registration.find("0x54, 0x45, 0x53, 0x54, 0x4f, 0x56, 0x4c, 0x31,") != std::string::npos,
                     "the signature bytes come from the image");
            t.IsTrue(registration.find("0x400000u, // load address") != std::string::npos, "load address recorded");
            t.IsTrue(registration.find("0x40u, // image size") != std::string::npos, "image size defaults to the file size");
            t.IsTrue(registration.find("0x0u, // signature offset") != std::string::npos, "signature offset recorded");
            t.IsTrue(registration.find("bool ps2x_register_overlay_unit_ovl()") != std::string::npos, "explicit registration function");
            t.IsTrue(registration.find("ps2_overlays::CatalogRegistrar") != std::string::npos, "passive catalog entry");
            t.IsTrue(registration.find("g_ps2RecompiledFunctionTable") == std::string::npos, "the dense table is untouched");
            t.IsTrue(registrationHeader.find("bool ps2x_register_overlay_unit_ovl();") != std::string::npos,
                     "the registration header declares the function");

            std::error_code ignored;
            std::filesystem::remove_all(dir, ignored);
        });

        tc.Run("per-function overlay files carry the overlay prefix", [](TestCase &t)
        {
            const std::filesystem::path dir = uniqueTempDir("ps2recomp-overlay-files");
            const std::filesystem::path image = dir / "unit.bin";
            const std::filesystem::path out = dir / "out";
            const std::filesystem::path config = dir / "overlay.toml";
            t.IsTrue(writeBytes(image, makeRawOverlayImage()) &&
                         writeOverlayConfig(config, image, out, false, "image_size = 0x80\nsignature_offset = 4\nsignature_size = 4\n"),
                     "inputs are written");
            t.IsTrue(recompileOverlay(config), "the raw image recompiles to per-function files");
            size_t functionFiles = 0u;
            bool allPrefixed = true;
            bool allNamespaced = true;
            for (const auto &item : std::filesystem::directory_iterator(out))
            {
                const std::string name = item.path().filename().string();
                allPrefixed = allPrefixed && name.rfind("unit_ovl_", 0) == 0;
                if (item.path().extension() == ".cpp" && name != "unit_ovl_overlay.cpp")
                {
                    ++functionFiles;
                    const std::string text = readText(item.path());
                    allNamespaced = allNamespaced && text.find("namespace ps2x_overlay_unit_ovl {") != std::string::npos &&
                                    text.find("#include \"unit_ovl_stubs.h\"") != std::string::npos;
                }
            }
            t.IsTrue(functionFiles >= 2u, "one source per function");
            t.IsTrue(allPrefixed, "every generated file name starts with the overlay name");
            t.IsTrue(allNamespaced, "every function source opens the overlay namespace");
            const std::string registration = readText(out / "unit_ovl_overlay.cpp");
            t.IsTrue(registration.find("0x80u, // image size") != std::string::npos, "configured image size is used");
            t.IsTrue(registration.find("0x4u, // signature offset") != std::string::npos, "configured signature offset is used");
            t.IsTrue(registration.find("0x4f, 0x56, 0x4c, 0x31,") != std::string::npos, "the signature is read at the offset");

            std::error_code ignored;
            std::filesystem::remove_all(dir, ignored);
        });

        tc.Run("an ELF input in overlay mode uses its sections", [](TestCase &t)
        {
            const std::filesystem::path dir = uniqueTempDir("ps2recomp-overlay-elf");
            const std::filesystem::path elfPath = dir / "unit.elf";
            const std::filesystem::path out = dir / "out";
            const std::vector<char> raw = makeRawOverlayImage();
            ELFIO::elfio writer;
            writer.create(ELFIO::ELFCLASS32, ELFIO::ELFDATA2LSB);
            writer.set_type(ELFIO::ET_EXEC);
            writer.set_machine(ELFIO::EM_MIPS);
            writer.set_entry(kBase + 0x10u);
            ELFIO::section *head = writer.sections.add(".data");
            head->set_type(ELFIO::SHT_PROGBITS);
            head->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_WRITE);
            head->set_address(kBase);
            head->set_data(raw.data(), 0x10u);
            ELFIO::section *text = writer.sections.add(".text");
            text->set_type(ELFIO::SHT_PROGBITS);
            text->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_EXECINSTR);
            text->set_addr_align(4);
            text->set_address(kBase + 0x10u);
            text->set_data(raw.data() + 0x10u, static_cast<ELFIO::Elf_Word>(raw.size() - 0x10u));
            t.IsTrue(writer.save(elfPath.string()), "overlay ELF written");

            const std::filesystem::path config = dir / "overlay.toml";
            {
                std::ofstream file(config);
                file << "[general]\ninput = \"" << elfPath.generic_string() << "\"\noutput = \"" << out.generic_string()
                     << "\"\nsingle_file_output = true\noutput_worker_threads = 1\n\n[overlay]\nname = \"elf_ovl\"\n"
                     << "load_address = 0x400000\nsignature_size = 8\n";
            }
            t.IsTrue(recompileOverlay(config), "the overlay ELF recompiles");
            const std::string registration = readText(out / "elf_ovl_overlay.cpp");
            t.IsTrue(registration.find("0x54, 0x45, 0x53, 0x54, 0x4f, 0x56, 0x4c, 0x31,") != std::string::npos,
                     "the signature is read from the ELF's data section");
            t.IsTrue(registration.find("0x40u, // image size") != std::string::npos, "image size spans the sections");
            t.IsTrue(registration.find("{0x400010u, &") != std::string::npos, "the ELF entry is registered");

            std::error_code ignored;
            std::filesystem::remove_all(dir, ignored);
        });

        tc.Run("overlay inputs outside their image are rejected", [](TestCase &t)
        {
            const std::filesystem::path dir = uniqueTempDir("ps2recomp-overlay-reject");
            const std::filesystem::path image = dir / "unit.bin";
            t.IsTrue(writeBytes(image, makeRawOverlayImage()), "image written");
            auto initializes = [&](const std::string &extra, const std::string &tag)
            {
                const std::filesystem::path config = dir / (tag + ".toml");
                writeOverlayConfig(config, image, dir / ("out_" + tag), true, kSignature8 + extra);
                PS2Recompiler recompiler(config.string());
                return recompiler.initialize();
            };
            t.IsTrue(initializes("", "ok"), "the reference configuration initializes");
            t.IsFalse(initializes("image_size = 0x20\n", "small"), "functions beyond image_size are rejected");
            t.IsFalse(initializes("signature_offset = 0x3C\n", "sig"), "a signature past the image end is rejected");
            t.IsFalse(initializes("code_size = 0x100\n", "code"), "a code range past the file end is rejected");
            t.IsFalse(initializes("entry = 0x400004\n", "entry"), "an entry outside the code range is rejected");
            t.IsFalse(initializes("code_size = 0x6\n", "odd"), "a code size that is not a word multiple is rejected");
            std::error_code ignored;
            std::filesystem::remove_all(dir, ignored);
        });
    });
}
