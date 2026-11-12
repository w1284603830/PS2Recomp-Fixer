#include "ps2_overlays.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

namespace ps2_overlays
{
    namespace detail
    {
        std::atomic<uint32_t> g_coverLow{0u};
        std::atomic<uint32_t> g_coverSpan{0u};
    }

    namespace
    {
        struct StoredImage
        {
            std::string name;
            uint32_t loadAddress = 0u;
            uint32_t imageEnd = 0u; // loadAddress + imageSize (fits: checked against kGuestRamSize)
            uint32_t signatureOffset = 0u;
            std::vector<uint8_t> signature;
            std::vector<Entry> entries;
        };

        struct Area
        {
            uint32_t loadAddress = 0u;
            uint32_t coverEnd = 0u; // largest imageEnd of the area's images
            std::vector<StoredImage> images;
            mutable std::atomic<int32_t> resident{-1}; // index of the image that matched at the last lookup
        };

        struct Registry
        {
            std::mutex mutex; // registration / clear; lookups do not lock (see ps2_overlays.h)
            std::vector<std::unique_ptr<Area>> areas; // highest load address first
        };

        Registry &registry()
        {
            static Registry instance;
            return instance;
        }

        struct Catalog
        {
            std::mutex mutex;
            std::vector<Image> images;
        };

        Catalog &catalog()
        {
            static Catalog instance; // filled from static initialisers: construct on first use
            return instance;
        }

        constexpr uint32_t kLoggedSwitchLimit = 1000u;
        std::atomic<uint32_t> g_loggedSwitches{0u};

        bool signatureMatches(const StoredImage &image, const uint8_t *rdram)
        {
            return std::memcmp(rdram + image.loadAddress + image.signatureOffset, image.signature.data(),
                               image.signature.size()) == 0;
        }

        // Some RAM contents match both signatures unless they overlap in at least one byte where they differ.
        bool signaturesExclusive(const StoredImage &a, const StoredImage &b)
        {
            const uint32_t aEnd = a.signatureOffset + static_cast<uint32_t>(a.signature.size());
            const uint32_t bEnd = b.signatureOffset + static_cast<uint32_t>(b.signature.size());
            const uint32_t first = std::max(a.signatureOffset, b.signatureOffset);
            const uint32_t last = std::min(aEnd, bEnd);
            for (uint32_t offset = first; offset < last; ++offset)
            {
                if (a.signature[offset - a.signatureOffset] != b.signature[offset - b.signatureOffset])
                    return true;
            }
            return false;
        }

        void logSwitch(const Area &area, int32_t index)
        {
            const uint32_t count = g_loggedSwitches.fetch_add(1u, std::memory_order_relaxed);
            if (count < kLoggedSwitchLimit)
            {
                std::fprintf(stderr, "[ps2-overlay] 0x%08X now %s\n", area.loadAddress,
                             index >= 0 ? area.images[static_cast<size_t>(index)].name.c_str()
                                        : "<no registered image>");
            }
            else if (count == kLoggedSwitchLimit)
            {
                std::fprintf(stderr, "[ps2-overlay] further resident-image changes are not logged\n");
            }
        }

        int32_t residentIndex(const Area &area, const uint8_t *rdram)
        {
            int32_t cached = area.resident.load(std::memory_order_relaxed);
            if (cached >= 0 && signatureMatches(area.images[static_cast<size_t>(cached)], rdram))
                return cached;
            int32_t found = -1;
            for (size_t i = 0; i < area.images.size(); ++i)
            {
                if (static_cast<int32_t>(i) != cached && signatureMatches(area.images[i], rdram))
                {
                    found = static_cast<int32_t>(i);
                    break;
                }
            }
            if (found != cached && area.resident.compare_exchange_strong(cached, found, std::memory_order_relaxed))
                logSwitch(area, found);
            return found;
        }

        void publishBounds(const Registry &reg)
        {
            uint32_t low = std::numeric_limits<uint32_t>::max();
            uint32_t high = 0u;
            for (const auto &area : reg.areas)
            {
                low = std::min(low, area->loadAddress);
                high = std::max(high, area->coverEnd);
            }
            if (reg.areas.empty())
            {
                low = 0u;
                high = 0u;
            }
            // Span first to 0, so a concurrent reader never pairs a new low with an old span.
            detail::g_coverSpan.store(0u, std::memory_order_relaxed);
            detail::g_coverLow.store(low, std::memory_order_relaxed);
            detail::g_coverSpan.store(high - low, std::memory_order_release);
        }

        Status validate(const Image &image)
        {
            if (image.name == nullptr || image.name[0] == '\0' || image.signature == nullptr ||
                image.signatureSize == 0u || image.signatureSize > kMaxSignatureSize || image.imageSize == 0u ||
                image.entries == nullptr || image.entryCount == 0u)
            {
                return Status::InvalidImage;
            }
            const uint64_t imageEnd = static_cast<uint64_t>(image.loadAddress) + image.imageSize;
            const uint64_t signatureEnd = static_cast<uint64_t>(image.signatureOffset) + image.signatureSize;
            if (imageEnd > kGuestRamSize || signatureEnd > image.imageSize)
                return Status::InvalidImage;
            for (uint32_t i = 0; i < image.entryCount; ++i)
            {
                const Entry &entry = image.entries[i];
                if (entry.function == nullptr || (entry.address & 3u) != 0u || entry.address < image.loadAddress ||
                    entry.address >= imageEnd || (i > 0u && entry.address <= image.entries[i - 1u].address))
                {
                    return Status::BadEntries;
                }
            }
            return Status::Ok;
        }
    }

    const char *describe(Status status)
    {
        switch (status)
        {
        case Status::Ok:
            return "ok";
        case Status::InvalidImage:
            return "invalid image description";
        case Status::BadEntries:
            return "function entries unsorted, duplicate, unaligned, null or outside the image";
        case Status::DuplicateName:
            return "an image with this name is already registered";
        case Status::AmbiguousSignature:
            return "signature is not exclusive of another image at the same load address";
        }
        return "unknown";
    }

    Status registerImage(const Image &image)
    {
        const Status valid = validate(image);
        if (valid != Status::Ok)
            return valid;

        StoredImage stored;
        stored.name = image.name;
        stored.loadAddress = image.loadAddress;
        stored.imageEnd = image.loadAddress + image.imageSize;
        stored.signatureOffset = image.signatureOffset;
        stored.signature.assign(image.signature, image.signature + image.signatureSize);
        stored.entries.assign(image.entries, image.entries + image.entryCount);

        Registry &reg = registry();
        std::lock_guard<std::mutex> lock(reg.mutex);
        Area *area = nullptr;
        for (const auto &candidate : reg.areas)
        {
            for (const StoredImage &existing : candidate->images)
            {
                if (existing.name == stored.name)
                    return Status::DuplicateName;
            }
            if (candidate->loadAddress == stored.loadAddress)
                area = candidate.get();
        }
        if (area != nullptr)
        {
            for (const StoredImage &existing : area->images)
            {
                if (!signaturesExclusive(existing, stored))
                    return Status::AmbiguousSignature;
            }
        }
        else
        {
            auto created = std::make_unique<Area>();
            created->loadAddress = stored.loadAddress;
            area = created.get();
            reg.areas.push_back(std::move(created));
            std::sort(reg.areas.begin(), reg.areas.end(),
                      [](const std::unique_ptr<Area> &a, const std::unique_ptr<Area> &b)
                      { return a->loadAddress > b->loadAddress; });
        }
        area->coverEnd = std::max(area->coverEnd, stored.imageEnd);
        area->images.push_back(std::move(stored));
        area->resident.store(-1, std::memory_order_relaxed);
        publishBounds(reg);
        return Status::Ok;
    }

    void clear()
    {
        Registry &reg = registry();
        std::lock_guard<std::mutex> lock(reg.mutex);
        detail::g_coverSpan.store(0u, std::memory_order_relaxed);
        reg.areas.clear();
        publishBounds(reg);
    }

    size_t registeredCount()
    {
        Registry &reg = registry();
        std::lock_guard<std::mutex> lock(reg.mutex);
        size_t count = 0u;
        for (const auto &area : reg.areas)
            count += area->images.size();
        return count;
    }

    Lookup resolve(const uint8_t *rdram, uint32_t address)
    {
        Lookup result;
        if (rdram == nullptr || (address & 3u) != 0u || !mayCover(address))
            return result;
        for (const auto &areaPtr : registry().areas)
        {
            const Area &area = *areaPtr;
            if (address < area.loadAddress || address >= area.coverEnd)
                continue;
            const int32_t index = residentIndex(area, rdram);
            if (index < 0)
                continue;
            const StoredImage &image = area.images[static_cast<size_t>(index)];
            if (address >= image.imageEnd)
                continue; // a lower load address may hold a larger image that covers the address
            result.image = image.name.c_str();
            const auto it = std::lower_bound(image.entries.begin(), image.entries.end(), address,
                                             [](const Entry &entry, uint32_t value) { return entry.address < value; });
            if (it != image.entries.end() && it->address == address)
            {
                result.resolution = Resolution::Found;
                result.function = it->function;
            }
            else
            {
                result.resolution = Resolution::Missing;
            }
            return result;
        }
        return result;
    }

    const char *residentImage(const uint8_t *rdram, uint32_t loadAddress)
    {
        if (rdram == nullptr)
            return nullptr;
        for (const auto &area : registry().areas)
        {
            if (area->loadAddress != loadAddress)
                continue;
            const int32_t index = residentIndex(*area, rdram);
            return index >= 0 ? area->images[static_cast<size_t>(index)].name.c_str() : nullptr;
        }
        return nullptr;
    }

    std::string describeResident(const uint8_t *rdram)
    {
        std::string out;
        for (const auto &area : registry().areas)
        {
            char address[16];
            std::snprintf(address, sizeof(address), "0x%08X=", area->loadAddress);
            if (!out.empty())
                out += ' ';
            out += address;
            const int32_t index = rdram ? residentIndex(*area, rdram) : -1;
            out += index >= 0 ? area->images[static_cast<size_t>(index)].name : std::string("-");
        }
        return out;
    }

    CatalogRegistrar::CatalogRegistrar(const Image &image)
    {
        Catalog &cat = catalog();
        std::lock_guard<std::mutex> lock(cat.mutex);
        cat.images.push_back(image);
    }

    size_t catalogSize()
    {
        Catalog &cat = catalog();
        std::lock_guard<std::mutex> lock(cat.mutex);
        return cat.images.size();
    }

    InstallReport installCatalog()
    {
        InstallReport report;
        Catalog &cat = catalog();
        std::lock_guard<std::mutex> lock(cat.mutex);
        for (const Image &image : cat.images)
        {
            const Status status = registerImage(image);
            if (status == Status::Ok)
            {
                ++report.installed;
                continue;
            }
            ++report.failed;
            std::fprintf(stderr, "[ps2-overlay] cannot register %s: %s\n", image.name ? image.name : "<unnamed>",
                         describe(status));
        }
        return report;
    }
}
