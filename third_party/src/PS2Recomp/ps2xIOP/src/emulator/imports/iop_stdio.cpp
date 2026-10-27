#include "iop_stdio.h"

#include "../core/iop_cpu.h"
#include "../core/iop_memory.h"
#include "ps2x/iop/iop_host.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace ps2x::iop::detail
{
    IopStdio::IopStdio(IopHost &host, IopMemory &memory) noexcept
        : m_host(host), m_memory(memory)
    {
    }

    bool IopStdio::dispatchImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const uint32_t a0 = cpu.gpr[4];
        const uint32_t a1 = cpu.gpr[5];
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };
        const auto logString = [&](std::string_view prefix, uint32_t address, uint32_t resultBias = 0u)
        {
            const std::string text = m_memory.readString(address, 2048u);
            m_host.log(LogLevel::Info, std::string(prefix) + text);
            setV0(static_cast<uint32_t>(text.size()) + resultBias);
        };

        // [DoD] printf with its arguments (o32: a1..a3, then the stack after the 16-byte register home area),
        // so driver logs such as "CRI ADX Driver : Main Thread Priority = %d" show their values.
        const auto formatPrintf = [&](uint32_t formatAddress, unsigned firstArg) -> std::string
        {
            const std::string format = m_memory.readString(formatAddress, 2048u);
            unsigned argIndex = firstArg;
            const auto nextArg = [&]() -> uint32_t
            {
                const unsigned i = argIndex++;
                return i < 4u ? cpu.gpr[4u + i] : m_memory.read32(cpu.gpr[29] + 16u + (i - 4u) * 4u);
            };
            std::string out;
            for (size_t p = 0; p < format.size(); ++p)
            {
                if (format[p] != '%')
                {
                    out += format[p];
                    continue;
                }
                size_t q = p + 1;
                while (q < format.size() && std::strchr("-+ #0123456789.lh", format[q]))
                    ++q;
                if (q >= format.size())
                    break;
                const char conv = format[q];
                std::string spec = format.substr(p, q - p);
                spec.erase(std::remove(spec.begin(), spec.end(), 'l'), spec.end());
                spec.erase(std::remove(spec.begin(), spec.end(), 'h'), spec.end());
                char buffer[512];
                switch (conv)
                {
                case '%':
                    out += '%';
                    break;
                case 's':
                    std::snprintf(buffer, sizeof(buffer), (spec + "s").c_str(), m_memory.readString(nextArg(), 256u).c_str());
                    out += buffer;
                    break;
                case 'c':
                    std::snprintf(buffer, sizeof(buffer), (spec + "c").c_str(), static_cast<int>(nextArg() & 0xFFu));
                    out += buffer;
                    break;
                case 'd':
                case 'i':
                    std::snprintf(buffer, sizeof(buffer), (spec + "d").c_str(), static_cast<int32_t>(nextArg()));
                    out += buffer;
                    break;
                case 'u':
                case 'x':
                case 'X':
                case 'o':
                    std::snprintf(buffer, sizeof(buffer), (spec + conv).c_str(), nextArg());
                    out += buffer;
                    break;
                case 'p':
                    std::snprintf(buffer, sizeof(buffer), "0x%08X", nextArg());
                    out += buffer;
                    break;
                default:
                    out.append(format, p, q - p + 1);
                    break;
                }
                p = q;
            }
            while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
                out.pop_back();
            return out;
        };

        switch (ordinal)
        {
        case 4: // printf
        {
            const std::string text = formatPrintf(a0, 1u);
            m_host.log(LogLevel::Info, "[IOP printf] " + text);
            setV0(static_cast<uint32_t>(text.size()));
            return true;
        }
        case 5: // getchar
        case 10:
            setV0(0xFFFFFFFFu);
            return true;
        case 6: // putchar
            m_host.log(LogLevel::Info, std::string("[IOP putchar] ") + static_cast<char>(a0 & 0xFFu));
            setV0(a0 & 0xFFu);
            return true;
        case 7: // puts
            logString("[IOP puts] ", a0, 1u);
            return true;
        case 8: // gets
        case 13:
            setV0(0u);
            return true;
        case 9: // fdprintf
            logString("[IOP fdprintf] ", a1);
            return true;
        case 11:
            setV0(a0 & 0xFFu);
            return true;
        case 12: // fdputs
            logString("[IOP fdputs] ", a0);
            return true;
        case 14: // vfdprintf
            logString("[IOP vfdprintf] ", a1);
            return true;
        default:
            return false;
        }
    }
}
