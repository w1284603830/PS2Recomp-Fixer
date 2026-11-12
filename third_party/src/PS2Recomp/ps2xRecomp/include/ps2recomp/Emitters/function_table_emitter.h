#ifndef PS2RECOMP_FUNCTION_TABLE_EMITTER_H
#define PS2RECOMP_FUNCTION_TABLE_EMITTER_H

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ps2recomp
{
    struct Function;
    class CodeGenerator;

    // What an [overlay] image registers with the runtime (ps2_overlays.h).
    struct OverlayImageInfo
    {
        std::string name;
        uint32_t loadAddress = 0;
        uint32_t imageSize = 0;
        uint32_t signatureOffset = 0;
        std::vector<uint8_t> signature;
    };

    class FunctionTableEmitter
    {
    public:
        explicit FunctionTableEmitter(CodeGenerator &codeGenerator);

        // Main ELF: the dense g_ps2RecompiledFunctionTable.
        std::string emit(const std::vector<Function> &functions, const std::map<uint32_t, std::string> &stubs);
        // Overlay: <name>_overlay.cpp (entries, signature, registration function, catalog entry) and its header.
        std::string emitOverlaySource(const std::vector<Function> &functions, const OverlayImageInfo &image);
        std::string emitOverlayHeader(const OverlayImageInfo &image);

    private:
        // Guest address -> generated function, sorted by address, one entry per address.
        std::vector<std::pair<uint32_t, std::string>> collectEntries(const std::vector<Function> &functions);

        CodeGenerator &m_codeGenerator;
    };
}

#endif // PS2RECOMP_FUNCTION_TABLE_EMITTER_H
