#ifndef PS2RECOMP_VU_TRANSLATOR_H
#define PS2RECOMP_VU_TRANSLATOR_H

#include <string>

namespace ps2recomp
{
    struct Instruction;
    class CodeGenerator;

    class VuTranslator
    {
    public:
        explicit VuTranslator(CodeGenerator &codeGenerator);
        std::string translate(const Instruction &inst);

    private:
        // [DoD] translate() = this + the shared macro-op epilogue (MAC flags, VF0 kept constant; upstream PR #292).
        std::string translateUnfixed(const Instruction &inst);
        CodeGenerator &m_codeGenerator;
    };
}

#endif // PS2RECOMP_VU_TRANSLATOR_H
