#include "ps2recomp/Emitters/function_table_emitter.h"
#include "ps2recomp/code_generator.h"
#include "ps2recomp/ps2_recompiler.h"
#include "ps2recomp/recompiler_reporter.h"
#include "ps2recomp/types.h"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ps2recomp
{
    FunctionTableEmitter::FunctionTableEmitter(CodeGenerator &codeGenerator)
        : m_codeGenerator(codeGenerator)
    {
    }

    std::vector<std::pair<uint32_t, std::string>> FunctionTableEmitter::collectEntries(const std::vector<Function> &functions)
    {
        CodeGenerator &cg = m_codeGenerator;
        std::vector<std::pair<uint32_t, std::string>> entries;
        std::unordered_set<uint32_t> registeredAddresses;

        auto addEntry = [&](uint32_t address, const std::string &name)
        {
            if (name.empty())
            {
                return;
            }
            if ((address & 3u) != 0u)
            {
                std::ostringstream oss;
                oss << "Unaligned function table entry for " << name << " at 0x" << std::hex << address;

                if (cg.m_reporter)
                {
                    cg.m_reporter->errorAt("function-table", name, address, oss.str());
                }

                throw std::runtime_error(oss.str());
            }
            if (!registeredAddresses.insert(address).second)
            {
                return;
            }
            entries.emplace_back(address, name);
        };

        std::vector<std::pair<uint32_t, std::string>> normalFunctions;
        std::vector<std::pair<uint32_t, std::string>> stubFunctions;
        std::vector<std::pair<uint32_t, std::string>> systemCallFunctions;
        std::vector<std::pair<uint32_t, std::string>> libraryFunctions;

        // Only synthetic, nested entry_* names may yield to a proven resume
        // owner. Named functions, standalone entries and HLE bindings retain
        // their own registrations. Choose the innermost owner deterministically.
        std::unordered_map<uint32_t, uint32_t> resumeOwners;
        for (const auto &[ownerStart, targets] : cg.m_resumeEntryTargetsByOwner)
        {
            for (uint32_t target : targets)
            {
                if (target == ownerStart)
                    continue;
                auto [it, inserted] = resumeOwners.emplace(target, ownerStart);
                if (!inserted && ownerStart > it->second)
                    it->second = ownerStart;
            }
        }

        for (const auto &function : functions)
        {
            if (!function.isRecompiled && !function.isStub && !function.isSkipped)
                continue;

            std::string generatedName = cg.getFunctionName(function.start);

            if (function.isSkipped)
            {
                libraryFunctions.emplace_back(function.start, generatedName);
            }
            else if (function.isStub)
            {
                const auto target = PS2Recompiler::resolveStubTarget(function.name);
                if (target == StubTarget::Syscall)
                {
                    systemCallFunctions.emplace_back(function.start, generatedName);
                }
                else
                {
                    stubFunctions.emplace_back(function.start, generatedName);
                }
            }
            else
            {
                if (function.name.rfind("entry_", 0) == 0)
                {
                    const auto owner = resumeOwners.find(function.start);
                    if (owner != resumeOwners.end())
                    {
                        const std::string ownerName = cg.getFunctionName(owner->second);
                        if (!ownerName.empty())
                            generatedName = ownerName;
                    }
                }
                normalFunctions.emplace_back(function.start, generatedName);
            }
        }

        if (cg.m_bootstrapInfo.valid)
        {
            std::string entryTarget = cg.m_bootstrapInfo.entryName;
            if (entryTarget.empty())
            {
                entryTarget = cg.getFunctionName(cg.m_bootstrapInfo.entry);
            }
            if (entryTarget.empty())
            {
                throw std::runtime_error("No entry function name available for registration.");
            }
            addEntry(cg.m_bootstrapInfo.entry, entryTarget);
        }

        for (const auto &[address, name] : normalFunctions)
        {
            addEntry(address, name);
        }

        for (const auto &[ownerStart, targets] : cg.m_resumeEntryTargetsByOwner)
        {
            const std::string ownerName = cg.getFunctionName(ownerStart);
            if (ownerName.empty())
            {
                continue;
            }

            for (uint32_t target : targets)
            {
                addEntry(target, ownerName);
            }
        }

        for (const auto &[address, name] : stubFunctions)
        {
            addEntry(address, name);
        }
        for (const auto &[address, name] : systemCallFunctions)
        {
            addEntry(address, name);
        }
        for (const auto &[address, name] : libraryFunctions)
        {
            addEntry(address, name);
        }

        std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b)
                  { return a.first < b.first; });
        return entries;
    }

    std::string FunctionTableEmitter::emit(const std::vector<Function> &functions, const std::map<uint32_t, std::string> &stubs)
    {
        (void)stubs;

        const std::vector<std::pair<uint32_t, std::string>> entries = collectEntries(functions);

        uint32_t tableBase = 0u;
        uint32_t tableEnd = 0u;
        uint32_t slotCount = 0u;
        if (!entries.empty())
        {
            tableBase = entries.front().first & ~3u;
            tableEnd = (entries.back().first + 4u + 3u) & ~3u;
            slotCount = (tableEnd - tableBase) >> 2;
        }

        std::stringstream ss;
        ss << "#include \"ps2_runtime.h\"\n";
        ss << "#include <ps2_recompiled_functions.h>\n";
        ss << "#include \"ps2_stubs.h\"\n";
        ss << "#include <ps2_recompiled_stubs.h>\n";
        ss << "#include \"ps2_syscalls.h\"\n\n";

        ss << "extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x" << std::hex << tableBase << "u;\n";
        ss << "extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x" << std::hex << tableEnd << "u;\n";
        ss << "extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = " << std::dec << slotCount << "u;\n";
        ss << "PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[" << std::dec << (slotCount == 0u ? 1u : slotCount) << "u] = {};\n\n";

        ss << "namespace {\n";
        ss << "struct GeneratedFunctionTableInitializer {\n";
        ss << "    GeneratedFunctionTableInitializer() {\n";
        for (const auto &[address, name] : entries)
        {
            const uint32_t slot = (address - tableBase) >> 2;
            ss << "        g_ps2RecompiledFunctionTable[" << std::dec << slot << "] = " << name
               << "; // 0x" << std::hex << address << std::dec << "\n";
        }
        ss << "    }\n";
        ss << "};\n";
        ss << "static const GeneratedFunctionTableInitializer g_generatedFunctionTableInitializer;\n";
        ss << "}\n";

        return ss.str();
    }

    std::string FunctionTableEmitter::emitOverlaySource(const std::vector<Function> &functions, const OverlayImageInfo &image)
    {
        CodeGenerator &cg = m_codeGenerator;
        const std::vector<std::pair<uint32_t, std::string>> entries = collectEntries(functions);
        const uint64_t imageEnd = static_cast<uint64_t>(image.loadAddress) + image.imageSize;
        if (entries.empty())
        {
            throw std::runtime_error("Overlay " + image.name + " has no recompiled function to register.");
        }
        for (const auto &[address, name] : entries)
        {
            if (address < image.loadAddress || address >= imageEnd)
            {
                std::ostringstream oss;
                oss << "Overlay " << image.name << " entry " << name << " at 0x" << std::hex << address
                    << " lies outside the image [0x" << image.loadAddress << ", 0x" << imageEnd << ")";
                throw std::runtime_error(oss.str());
            }
        }

        const std::string ns = cg.overlayNamespace();
        std::stringstream ss;
        ss << "// Generated by ps2_recomp from an [overlay] configuration; do not edit.\n";
        ss << "// Overlay " << image.name << ": load address 0x" << std::hex << image.loadAddress << ", 0x"
           << image.imageSize << " bytes, signature of " << std::dec << image.signature.size()
           << " bytes at +0x" << std::hex << image.signatureOffset << std::dec << ".\n";
        ss << "// Nothing here is active until the port registers it after PS2Runtime::loadELF:\n";
        ss << "// ps2x_register_overlay_" << image.name << "() or ps2_overlays::installCatalog().\n";
        ss << "#include <cstdio>\n";
        ss << "#include \"ps2_overlays.h\"\n";
        ss << "#include \"ps2_runtime.h\"\n";
        ss << cg.includeGeneratedHeader(cg.functionsHeaderName());
        ss << cg.includeGeneratedHeader(cg.stubsHeaderName());
        ss << cg.includeGeneratedHeader(image.name + "_overlay.h") << "\n";

        ss << "namespace " << ns << " {\n";
        ss << "namespace {\n";
        ss << "const ps2_overlays::Entry kEntries[] = {\n";
        for (const auto &[address, name] : entries)
        {
            ss << "    {0x" << std::hex << address << std::dec << "u, &" << name << "},\n";
        }
        ss << "};\n";
        ss << "const uint8_t kSignature[] = {";
        for (size_t i = 0; i < image.signature.size(); ++i)
        {
            ss << (i % 16u == 0u ? "\n    " : " ") << "0x" << std::hex << static_cast<unsigned>(image.signature[i])
               << std::dec << ",";
        }
        ss << "\n};\n";
        ss << "} // namespace\n\n";
        ss << "extern const ps2_overlays::Image kImage = {\n";
        ss << "    \"" << image.name << "\",\n";
        ss << "    0x" << std::hex << image.loadAddress << "u, // load address\n";
        ss << "    0x" << image.imageSize << "u, // image size\n";
        ss << "    0x" << image.signatureOffset << "u, // signature offset\n" << std::dec;
        ss << "    kSignature,\n";
        ss << "    static_cast<uint32_t>(sizeof(kSignature)),\n";
        ss << "    kEntries,\n";
        ss << "    static_cast<uint32_t>(sizeof(kEntries) / sizeof(kEntries[0])),\n";
        ss << "};\n";
        ss << "} // namespace " << ns << "\n\n";

        ss << "bool ps2x_register_overlay_" << image.name << "()\n";
        ss << "{\n";
        ss << "    const ps2_overlays::Status status = ps2_overlays::registerImage(" << ns << "::kImage);\n";
        ss << "    if (status == ps2_overlays::Status::Ok)\n";
        ss << "        return true;\n";
        ss << "    std::fprintf(stderr, \"[ps2-overlay] cannot register " << image.name
           << ": %s\\n\", ps2_overlays::describe(status));\n";
        ss << "    return false;\n";
        ss << "}\n\n";

        ss << "// Passive catalog entry: link this file into the executable itself, not into a static library.\n";
        ss << "static const ps2_overlays::CatalogRegistrar g_ps2xOverlayCatalog_" << image.name << "(" << ns
           << "::kImage);\n";
        return ss.str();
    }

    std::string FunctionTableEmitter::emitOverlayHeader(const OverlayImageInfo &image)
    {
        std::stringstream ss;
        ss << "// Generated by ps2_recomp from an [overlay] configuration; do not edit.\n";
        ss << "#pragma once\n\n";
        ss << "#include \"ps2_overlays.h\"\n\n";
        ss << "namespace " << m_codeGenerator.overlayNamespace() << " {\n";
        ss << "extern const ps2_overlays::Image kImage;\n";
        ss << "}\n\n";
        ss << "// Registers overlay " << image.name << " now; call after PS2Runtime::loadELF (which clears the "
           << "registry).\n";
        ss << "// Returns false and prints the reason when the registry rejects it.\n";
        ss << "bool ps2x_register_overlay_" << image.name << "();\n";
        return ss.str();
    }
}
